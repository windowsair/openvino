// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <ie_metric_helpers.hpp>
#include <precision_utils.h>
#include "mkldnn_exec_network.h"

#include "mkldnn_async_infer_request.h"
#include "mkldnn_infer_request.h"
#include "mkldnn_memory_state.h"
#include "mkldnn_itt.h"
#include "mkldnn_serialize.h"
#include "ngraph/type/element_type.hpp"
#include "nodes/mkldnn_memory_node.hpp"
#include <threading/ie_executor_manager.hpp>
#define FIX_62820 0
#if FIX_62820 && ((IE_THREAD == IE_THREAD_TBB) || (IE_THREAD == IE_THREAD_TBB_AUTO))
#include <threading/ie_tbb_streams_executor.hpp>
#endif
#include <threading/ie_cpu_streams_executor.hpp>
#include <ie_system_conf.h>
#include <ngraph/opsets/opset1.hpp>
#include <transformations/utils/utils.hpp>
#include <ie_ngraph_utils.hpp>
#include "cpp_interfaces/interface/ie_iplugin_internal.hpp"
#include "ie_icore.hpp"
#include "openvino/runtime/properties.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <cstring>

using namespace MKLDNNPlugin;
using namespace InferenceEngine;
using namespace InferenceEngine::details;

InferenceEngine::IInferRequestInternal::Ptr
MKLDNNExecNetwork::CreateInferRequestImpl(const std::vector<std::shared_ptr<const ov::Node>>& inputs,
                                          const std::vector<std::shared_ptr<const ov::Node>>& outputs) {
    if (!this->_plugin)
        return nullptr;
    const auto& core = _plugin->GetCore();
    if (!core || !core->isNewAPI())
        return nullptr;
    return std::make_shared<MKLDNNInferRequest>(inputs, outputs, std::static_pointer_cast<MKLDNNExecNetwork>(shared_from_this()));
}

InferenceEngine::IInferRequestInternal::Ptr
MKLDNNExecNetwork::CreateInferRequestImpl(InferenceEngine::InputsDataMap networkInputs,
                                          InferenceEngine::OutputsDataMap networkOutputs) {
    return std::make_shared<MKLDNNLegacyInferRequest>(networkInputs, networkOutputs, std::static_pointer_cast<MKLDNNExecNetwork>(shared_from_this()));
}

struct ImmediateSerialExecutor : public ITaskExecutor {
    void run(InferenceEngine::Task task) override {
        std::lock_guard<std::mutex> l{_mutex};
        task();
    }
    std::mutex _mutex;
};

MKLDNNExecNetwork::MKLDNNExecNetwork(const InferenceEngine::CNNNetwork &network,
                                     const Config &cfg,
                                     const MKLDNNExtensionManager::Ptr& extMgr,
                                     NumaNodesWeights &numaNodesWeights,
                                     const std::shared_ptr<InferenceEngine::IInferencePlugin>& plugin) :
    InferenceEngine::ExecutableNetworkThreadSafeDefault{nullptr, nullptr},
    extensionManager(extMgr),
    _cfg{cfg},
    _name{network.getName()},
    _numaNodesWeights(numaNodesWeights),
    _network(network) {
    SetPointerToPlugin(plugin);
    auto function = network.getFunction();
    if (function == nullptr) {
        IE_THROW() << "CPU plug-in doesn't support not ngraph-based model!";
    }
    bool isFloatModel = !ngraph::op::util::has_op_with_type<ngraph::op::FakeQuantize>(function);

    _cfg.isNewApi = !isLegacyAPI();

    // WA for inference dynamic batch cases in new API
    if (_cfg.isNewApi) {
        int64_t maxBatchSize = -1;
        if (canBeExecViaLegacyDynBatch(function, maxBatchSize)) {
            IE_ASSERT(maxBatchSize > -1);
            _cfg.batchLimit = maxBatchSize;
        }
    } else if (_cfg.batchLimit > 1) {
        // check topology for applicability
        if (!CanProcessDynBatch(_network)) {
            IE_THROW() << "MKLDNNGraph::CreateGraph: such topology cannot be compiled for dynamic batch!";
        }
    }

    if (cfg.exclusiveAsyncRequests) {
        // special case when all InferRequests are muxed into a single queue
        _taskExecutor = _plugin->executorManager()->getExecutor("CPU");
    } else {
        auto streamsExecutorConfig = InferenceEngine::IStreamsExecutor::Config::MakeDefaultMultiThreaded(_cfg.streamExecutorConfig, isFloatModel);
        streamsExecutorConfig._name = "CPUStreamsExecutor";
#if FIX_62820 && (IE_THREAD == IE_THREAD_TBB || IE_THREAD == IE_THREAD_TBB_AUTO)
        _taskExecutor = std::make_shared<TBBStreamsExecutor>(streamsExecutorConfig);
#else
        _taskExecutor = _plugin->executorManager()->getIdleCPUStreamsExecutor(streamsExecutorConfig);
#endif
    }
    if (0 != cfg.streamExecutorConfig._streams) {
#if FIX_62820 && (IE_THREAD == IE_THREAD_TBB || IE_THREAD == IE_THREAD_TBB_AUTO)
        // There is no additional threads but we still need serialize callback execution to preserve legacy behaviour
        _callbackExecutor = std::make_shared<ImmediateSerialExecutor>();
#else
        _callbackExecutor = _plugin->executorManager()->getIdleCPUStreamsExecutor(
                                IStreamsExecutor::Config{"CPUCallbackExecutor", 1, 0, IStreamsExecutor::ThreadBindingType::NONE});
#endif
    } else {
        _callbackExecutor = _taskExecutor;
    }

    int streams = std::max(1, _cfg.streamExecutorConfig._streams);
    std::vector<Task> tasks; tasks.resize(streams);
    _graphs.resize(streams);
    if (_cfg.streamExecutorConfig._streams != 0) {
        for (auto&& task : tasks) {
            task = [this] {
                MKLDNNExecNetwork::GetGraph();
            };
        }
        _taskExecutor->runAndWait(tasks);
    } else {
        MKLDNNExecNetwork::GetGraph();
    }

    // Save all MemoryLayer data tensors. Will use insight about mechanics
    // of MemoryLayer implementation. It uses output edge of MemoryLayer
    // producer as storage for tensor to keep it between infer calls.
    if (_graphs.size() == 1) {
        for (auto &node : GetGraph()._graph.GetNodes()) {
            if (node->getType() == MemoryInput) {
                auto memoryNode = dynamic_cast<MKLDNNMemoryInputNode*>(node.get());
                if (!memoryNode) {
                    IE_THROW() << "Cannot cast " << node->getName() << " to MKLDNNMemoryInputNode";
                }
                auto state_store = memoryNode->getStore();
                auto state_name = memoryNode->getId();

                // Remove suffix with pair ID. Internal information.
                auto suffix_idx = state_name.find("/id=");
                if (suffix_idx != std::string::npos)
                    state_name = state_name.substr(0, suffix_idx);

                memoryStates.emplace_back(new MKLDNNVariableState(state_name, state_store));
            }
        }
    }
}

MKLDNNExecNetwork::Graph::Lock MKLDNNExecNetwork::GetGraph() const {
    int streamId = 0;
    int numaNodeId = 0;
    auto streamsExecutor = dynamic_cast<InferenceEngine::IStreamsExecutor*>(_taskExecutor.get());
    if (nullptr != streamsExecutor) {
        streamId = streamsExecutor->GetStreamId();
        numaNodeId = streamsExecutor->GetNumaNodeId();
    }
    auto graphLock = Graph::Lock(_graphs[streamId % _graphs.size()]);
    if (!graphLock._graph.IsReady()) {
        std::exception_ptr exception;
        auto makeGraph = [&] {
            try {
                {
                    std::lock_guard<std::mutex> lock{_cfgMutex};
                    graphLock._graph.setConfig(_cfg);
                }
                graphLock._graph.CreateGraph(_network, extensionManager, _numaNodesWeights[numaNodeId]);
            } catch(...) {
                exception = std::current_exception();
            }
        };
        if (nullptr != streamsExecutor) {
            streamsExecutor->Execute(makeGraph);
        } else {
            makeGraph();
        }
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
    return graphLock;
}

void MKLDNNExecNetwork::setProperty(const std::map<std::string, std::string> &properties) {
    {
        std::lock_guard<std::mutex> lock{_cfgMutex};
        _cfg.readProperties(properties);
    }
    for (auto& g : _graphs) {
        auto graphLock = Graph::Lock(g);
        if (graphLock._graph.IsReady()) {
            graphLock._graph.setProperty(properties);
        }
    }
}

InferenceEngine::IInferRequestInternal::Ptr MKLDNNExecNetwork::CreateInferRequest() {
    return CreateAsyncInferRequestFromSync<MKLDNNAsyncInferRequest>();
}

std::shared_ptr<ngraph::Function> MKLDNNExecNetwork::GetExecGraphInfo() {
    if (_graphs.empty())
        IE_THROW() << "No graph was found";

    return GetGraph()._graph.dump();
}

bool MKLDNNExecNetwork::isLegacyAPI() const {
    const auto& core = _plugin->GetCore();
    if (!core)
        IE_THROW() << "Unable to get API version. Core is unavailable";

    return !core->isNewAPI();
}

Parameter MKLDNNExecNetwork::GetConfigLegacy(const std::string &name) const {
    if (_graphs.empty())
        IE_THROW() << "No graph was found";
    /* legacy implementation return all the parameters which is actually not correct
     * since they are not reconfigurable. Fixed for new API */
    Config engConfig = GetGraph()._graph.getProperty();
    auto option = engConfig._config.find(name);
    if (option != engConfig._config.end()) {
        return option->second;
    } else {
        IE_THROW() << "Unsupported ExecutableNetwork config key: " << name;
    }
}

/**
 * Only legacy parameters are supported.
 * No RW peroperties supported for new API.
 * All the RO properties are covered with GetMetric() method and
 * GetConfig() is not expected to be called by new API with params from new configuration API.
 */
Parameter MKLDNNExecNetwork::GetConfig(const std::string &name) const {
    /* Internally legacy parameters are used with new API as part of migration procedure.
     * This fallback can be removed as soon as migration completed */
    return GetConfigLegacy(name);
}

InferenceEngine::Parameter MKLDNNExecNetwork::GetMetricLegacy(const std::string &name, const Graph& graph) const {
    if (name == METRIC_KEY(NETWORK_NAME)) {
        IE_SET_METRIC_RETURN(NETWORK_NAME, graph.dump()->get_friendly_name());
    } else if (name == METRIC_KEY(SUPPORTED_METRICS)) {
        std::vector<std::string> metrics;
        metrics.push_back(METRIC_KEY(NETWORK_NAME));
        metrics.push_back(METRIC_KEY(SUPPORTED_METRICS));
        metrics.push_back(METRIC_KEY(SUPPORTED_CONFIG_KEYS));
        metrics.push_back(METRIC_KEY(OPTIMAL_NUMBER_OF_INFER_REQUESTS));
        IE_SET_METRIC_RETURN(SUPPORTED_METRICS, metrics);
    } else if (name == METRIC_KEY(SUPPORTED_CONFIG_KEYS)) {
        std::vector<std::string> configKeys;
        for (auto && key : graph.getProperty()._config) {
            configKeys.push_back(key.first);
        }
        IE_SET_METRIC_RETURN(SUPPORTED_CONFIG_KEYS, configKeys);
    } else if (name == METRIC_KEY(OPTIMAL_NUMBER_OF_INFER_REQUESTS)) {
        Config engConfig = graph.getProperty();
        auto option = engConfig._config.find(CONFIG_KEY(CPU_THROUGHPUT_STREAMS));
        IE_ASSERT(option != engConfig._config.end());
        auto streams = std::stoi(option->second);
        IE_SET_METRIC_RETURN(OPTIMAL_NUMBER_OF_INFER_REQUESTS, static_cast<unsigned int>(
            streams ? streams : 1));
    } else {
        IE_THROW() << "Unsupported ExecutableNetwork metric: " << name;
    }
}

InferenceEngine::Parameter MKLDNNExecNetwork::GetMetric(const std::string &name) const {
    if (_graphs.empty())
        IE_THROW() << "No graph was found";
    // @todo Can't we just use local copy (_cfg) instead?
    auto graphLock = GetGraph();
    const auto& graph = graphLock._graph;
    const auto& config = graph.getProperty();

    if (isLegacyAPI()) {
        return GetMetricLegacy(name, graph);
    }

    auto RO_property = [](const std::string& propertyName) {
        return ov::PropertyName(propertyName, ov::PropertyMutability::RO);
    };

    if (name == ov::supported_properties) {
        return std::vector<ov::PropertyName> {
            RO_property(ov::supported_properties.name()),
            RO_property(ov::model_name.name()),
            RO_property(ov::optimal_number_of_infer_requests.name()),
            RO_property(ov::num_streams.name()),
            RO_property(ov::affinity.name()),
            RO_property(ov::inference_num_threads.name()),
            RO_property(ov::enable_profiling.name()),
            RO_property(ov::hint::inference_precision.name()),
            RO_property(ov::hint::performance_mode.name()),
            RO_property(ov::hint::num_requests.name()),
        };
    }

    if (name == ov::model_name) {
        // @todo Does not seem ok to 'dump()' the whole graph everytime in order to get a name
        return graph.dump()->get_friendly_name();
    } else if (name == ov::optimal_number_of_infer_requests) {
        const auto streams = config.streamExecutorConfig._streams;
        return static_cast<uint32_t>(streams); // ov::optimal_number_of_infer_requests has no negative values
    } else if (name == ov::num_streams) {
        const auto streams = config.streamExecutorConfig._streams;
        return static_cast<int32_t>(streams); // ov::num_streams has special negative values (AUTO = -1, NUMA = -2)
    } else if (name == ov::affinity) {
        const auto affinity = config.streamExecutorConfig._threadBindingType;
        switch (affinity) {
        case InferenceEngine::IStreamsExecutor::ThreadBindingType::NONE:
            return ov::Affinity::NONE;
        case InferenceEngine::IStreamsExecutor::ThreadBindingType::CORES:
            return ov::Affinity::CORE;
        case InferenceEngine::IStreamsExecutor::ThreadBindingType::NUMA:
            return ov::Affinity::NUMA;
        case InferenceEngine::IStreamsExecutor::ThreadBindingType::HYBRID_AWARE:
            return ov::Affinity::HYBRID_AWARE;
        }
        return ov::Affinity::NONE;
    } else if (name == ov::inference_num_threads) {
        const auto num_threads = config.streamExecutorConfig._threads;
        return num_threads;
    } else if (name == ov::enable_profiling.name()) {
        const bool perfCount = config.collectPerfCounters;
        return perfCount ? "YES" : "NO";
    } else if (name == ov::hint::inference_precision) {
        const auto enforceBF16 = config.enforceBF16;
        return enforceBF16 ? ov::element::bf16 : ov::element::f32;
    } else if (name == ov::hint::performance_mode) {
        const auto perfHint = config.perfHintsConfig.ovPerfHint;
        return perfHint;
    } else if (name == ov::hint::num_requests) {
        const auto perfHintNumRequests = config.perfHintsConfig.ovPerfHintNumRequests;
        return perfHintNumRequests;
    }
    /* Internally legacy parameters are used with new API as part of migration procedure.
     * This fallback can be removed as soon as migration completed */
    return GetMetricLegacy(name, graph);
}

bool MKLDNNExecNetwork::canBeExecViaLegacyDynBatch(std::shared_ptr<const ov::Model> function, int64_t& maxBatchSize) const {
    maxBatchSize = -1;
    auto isDynBatchWithUpperBound = [maxBatchSize](const ov::PartialShape& shape) -> bool {
        if (shape.rank().is_dynamic()) {
            return false;
        }

        bool retVal = shape[0].is_dynamic() && shape[0].get_max_length() != maxBatchSize;
        for (size_t i = 1; i < shape.size(); i++) {
            retVal = retVal && shape[i].is_static();
        }
        return retVal;
    };

    if (function->get_parameters().size() != 1) {
        return false;
    }

    auto param = *function->get_parameters().begin();
    const auto shape = param->get_output_partial_shape(0);
    if (shape.rank().is_dynamic()) {
        return false;
    }

    if (shape.rank().get_length() < 2) {
        return false;
    } else {
        if (maxBatchSize == -1) {
            maxBatchSize = shape[0].get_max_length();

            if (maxBatchSize == -1) {
                return false;
            }
        }

        if (!isDynBatchWithUpperBound(shape)) {
            return false;
        }
    }

    auto ops = function->get_ordered_ops();
    for (auto op : ops) {
        if (op->get_type_info() == ngraph::op::Constant::get_type_info_static()) {
            continue;
        }

        auto type = TypeFromName(op->get_type_name());
        if (!one_of(type, Input,
                          Output,
                          Convolution,
                          Deconvolution,
                          Lrn,
                          Pooling,
                          FullyConnected,
                          MatMul,
                          Softmax,
                          Split,
                          Concatenation,
                          Eltwise,
                          Reshape,
                          Tile)) {
            return false;
        }

        for (size_t i = 0; i < op->get_output_size(); i++) {
            if (!isDynBatchWithUpperBound(op->get_output_partial_shape(i))) {
                return false;
            }
        }

        if (type == Tile) {
            const auto tile = std::dynamic_pointer_cast<const ngraph::opset1::Tile>(op);
            const auto repeatsNode = std::dynamic_pointer_cast<const ngraph::opset1::Constant>(tile->get_input_node_shared_ptr(1));

            if (!(tile && repeatsNode && repeatsNode->cast_vector<int64_t>()[0] == 1)) {
                return false;
            }
        }

        if (type == Reshape) {
            const auto inShape = op->get_input_partial_shape(0);
            const auto outShape = op->get_output_partial_shape(0);
            if (isDynBatchWithUpperBound(inShape) && isDynBatchWithUpperBound(outShape)) {
                size_t inSize = 1, outSize = 1;
                for (size_t i = 1; i < inShape.size(); i++) {
                    inSize *= inShape[i].get_length();
                }
                for (size_t i = 1; i < outShape.size(); i++) {
                    outSize *= outShape[i].get_length();
                }

                if (inSize != outSize) {
                    return false;
                }
            } else {
                return false;
            }
        }

        if (type == Split) {
            const auto axis = std::dynamic_pointer_cast<const ngraph::opset1::Constant>(op->get_input_node_shared_ptr(1));
            if (!axis || axis->cast_vector<int64_t>()[0] == 0) {
                return false;
            }
        }

        if (type == Concatenation) {
            const auto concat = std::dynamic_pointer_cast<const ngraph::op::v0::Concat>(op);
            if (!concat || concat->get_axis() == 0) {
                return false;
            }
        }

        if (type == Softmax) {
            const auto softmax = std::dynamic_pointer_cast<const ngraph::opset1::Softmax>(op);
            if (!softmax || softmax->get_axis() == 0) {
                return false;
            }
        }

        if ((type == MatMul || type == FullyConnected) &&
            (op->get_input_node_ptr(1)->get_type_info() != ngraph::op::Constant::get_type_info_static() ||
                op->get_input_partial_shape(0).rank().get_length() < 2)) {
            return false;
        }

        if (type == Eltwise && std::dynamic_pointer_cast<ov::op::util::BinaryElementwiseArithmetic>(op) &&
            !(op->get_input_node_ptr(0)->get_type_info() == ngraph::op::Constant::get_type_info_static() ||
            op->get_input_node_ptr(1)->get_type_info() == ngraph::op::Constant::get_type_info_static()) &&
                    op->get_input_partial_shape(0).rank().get_length() != op->get_input_partial_shape(1).rank().get_length()) {
                return false;
        }
    }
    return true;
}

bool MKLDNNExecNetwork::CanProcessDynBatch(const InferenceEngine::CNNNetwork &network) const {
    InputsDataMap inputs = network.getInputsInfo();

    if (inputs.empty())
        return false;

    auto function = network.getFunction();
    if (function == nullptr) {
        IE_THROW() << "CPU plug-in doesn't support not ngraph-based model!";
    }

    auto ops = function->get_ordered_ops();
    for (const auto& op : ops) {
        auto type = TypeFromName(op->get_type_name());
        if (type == Tile) {
            const auto tile = std::dynamic_pointer_cast<const ngraph::opset1::Tile>(op);
            const auto repeatsNode = std::dynamic_pointer_cast<const ngraph::opset1::Constant>(tile->get_input_node_shared_ptr(1));
            if (!repeatsNode)
                return false;
            if (tile && repeatsNode->cast_vector<int64_t>()[0] == 1)
                continue;
        }

        if (type == Reshape) {
            if (op->get_input_shape(0)[0] == op->get_output_shape(0)[0])
                continue;
        }

        if (type != Input &&
            type != Output &&
            type != Convolution &&
            type != Deconvolution &&
            type != Lrn &&
            type != Pooling &&
            type != FullyConnected &&
            type != MatMul &&
            type != Softmax &&
            type != Split &&
            type != Concatenation &&
                type != Eltwise) {
            return false;
        }
    }

    return true;
}

void MKLDNNExecNetwork::Export(std::ostream& modelStream) {
    CNNNetworkSerializer serializer(modelStream, extensionManager);
    serializer <<_network;
}
