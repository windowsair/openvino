// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <mkldnn_node.h>
#include "kernels/gather_uni_kernel.hpp"

#include <memory>
#include <string>
#include <vector>

namespace MKLDNNPlugin {

class MKLDNNGatherNode : public MKLDNNNode {
public:
    MKLDNNGatherNode(const std::shared_ptr<ngraph::Node>& op, const mkldnn::engine& eng, MKLDNNWeightsSharing::Ptr &cache);

    void getSupportedDescriptors() override {};
    void initSupportedPrimitiveDescriptors() override;
    void createPrimitive() override;
    void execute(mkldnn::stream strm) override;
    bool created() const override;

    static bool isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept;

    struct threadExecParams {
        std::vector<int> specIdxInBytes;
        std::vector<int> permIdxMask;
        std::vector<int> srcBeforeAxisDiff;
        std::vector<int> idxBatchSumInBytes;
        std::vector<int> dataBeforeAxisSumInBytes;

        std::vector<int> afterAxIdxInBytes;
        std::vector<int> specIdxDiff;
        std::vector<int> beforeAxPermMask;
        std::vector<int> afterAxPermMask;
        int betweenBatchAndAxisIter = 0;
        int specIdxAndAfterAxIterB = 0;

        uint64_t workAmount = 0;
        uint64_t dstStart = 0;
    };

protected:
    void executeDynamicImpl(mkldnn::stream strm) override;
    bool needPrepareParams() const override;
    void prepareParams() override;
    std::vector<VectorDims> shapeInfer() const override;

private:
    void initShortParams(threadExecParams& p, uint64_t start);
    void execReference();

    bool isDataShapeStat = false;
    bool isIdxShapeStat = false;
    bool isAxisInputConst = false;

    bool reverseIndexing = false;

    uint64_t dataTypeSize = 1lu;
    static constexpr uint64_t idxTypeSize = sizeof(int);

    int axis = 0;
    int axisDim = 0;
    int batchDims = 0;
    int dataSrcRank = 1;
    uint64_t specIndicesSize = 0lu;
    uint64_t beforeBatchSize = 0lu;
    uint64_t beforeAxisSize = 0lu;
    uint64_t betweenBatchAndAxisSize = 0lu;
    uint64_t afterAxisSize = 0lu;
    uint64_t afterAxisSizeInBytes = 0lu;
    uint64_t axisAndAfterAxisSizeInBytes = 0lu;
    uint64_t srcAfterBatchSizeInBytes = 0lu;
    uint64_t specIdxAndAfterAxSizeB = 0lu;
    uint64_t totalWork = 0lu;

    std::vector<threadExecParams> execParamsPerThread;

    static constexpr size_t GATHER_DATA = 0;
    static constexpr size_t GATHER_INDICES = 1;
    static constexpr size_t GATHER_AXIS = 2;

    std::shared_ptr<jitGatherKernelBase> jitKernel;
};

}  // namespace MKLDNNPlugin
