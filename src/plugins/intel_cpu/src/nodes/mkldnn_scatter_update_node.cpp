// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_scatter_update_node.h"
#include <mkldnn.hpp>
#include <string>
#include <vector>
#include <mkldnn_types.h>
#include <mkldnn_extension_utils.h>
#include "ie_parallel.hpp"
#include <algorithm>
#include "common/cpu_memcpy.h"

#include <ngraph/opsets/opset3.hpp>
#include <ngraph/opsets/opset4.hpp>

using namespace mkldnn;
using namespace MKLDNNPlugin;
using namespace InferenceEngine;

bool MKLDNNScatterUpdateNode::isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept {
    try {
        auto scatterElemUpd = ngraph::as_type_ptr<const ngraph::opset3::ScatterElementsUpdate>(op);
        auto scatterUpd = ngraph::as_type_ptr<const ngraph::opset3::ScatterUpdate>(op);
        auto scatterNdUpd = ngraph::as_type_ptr<const ngraph::opset4::ScatterNDUpdate>(op);
        if (!scatterElemUpd && !scatterUpd && !scatterNdUpd) {
            const std::string opType = op->get_type_name();
            errorMessage = "Only opset" + opType == "ScatterNDUpdate" ? "4 " : "3 " + opType + " operation is supported";
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool MKLDNNScatterUpdateNode::isExecutable() const {
    return !isInputTensorAtPortEmpty(DATA_ID);
}

MKLDNNScatterUpdateNode::MKLDNNScatterUpdateNode(const std::shared_ptr<ngraph::Node>& op, const mkldnn::engine& eng, MKLDNNWeightsSharing::Ptr &cache)
        : MKLDNNNode(op, eng, cache), dataSize(0lu), indicesSize(0lu), axisSize(0lu), dataPrec(Precision::UNSPECIFIED), indicesPrec(Precision::UNSPECIFIED),
          axisPrec(Precision::UNSPECIFIED) {
    std::string errorMessage;
    if (isSupportedOperation(op, errorMessage)) {
        errorPrefix = std::string(op->get_type_name()) + " node with name '" + getName() + "'";
    } else {
        IE_THROW(NotImplemented) << errorMessage;
    }
}

void MKLDNNScatterUpdateNode::getSupportedDescriptors() {
    if ((getParentEdges().size() != 3) && (getParentEdges().size() != 4))
        IE_THROW() << errorPrefix << " has incorrect number of input edges";
    if (getChildEdges().empty())
        IE_THROW() << errorPrefix << " has incorrect number of output edges";

    if (getInputShapeAtPort(DATA_ID).getRank() < 1 ||
        getInputShapeAtPort(INDICES_ID).getRank() < 1 ||
            getInputShapeAtPort(UPDATE_ID).getRank() < 1) {
        IE_THROW() << errorPrefix << " do not support scalar input";
    }

    Type scatterUpdateType = getType();
    if (scatterUpdateType == ScatterUpdate) {
        scatterUpdateMode = ScatterUpdateMode::ScatterUpdate;
        axisRelaxed = true;
    } else if (scatterUpdateType == ScatterElementsUpdate) {
        scatterUpdateMode = ScatterUpdateMode::ScatterElementsUpdate;
        axisRelaxed = true;
    } else if (scatterUpdateType == ScatterNDUpdate) {
        scatterUpdateMode = ScatterUpdateMode::ScatterNDUpdate;
        axisRelaxed = false;
    } else {
        IE_THROW() << errorPrefix << " is not supported";
    }
}

void MKLDNNScatterUpdateNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    const auto& srcDataDim = getInputShapeAtPort(DATA_ID).getDims();
    const auto& indicesDim = getInputShapeAtPort(INDICES_ID).getDims();
    const auto& updateDim =  getInputShapeAtPort(UPDATE_ID).getDims();
    const auto& dstDataDim = getOutputShapeAtPort(0).getDims();

    size_t srcRank = srcDataDim.size();
    size_t indicesRank = indicesDim.size();
    size_t updateRank = updateDim.size();
    size_t dstRank = dstDataDim.size();

    // common check
    if (srcRank != dstRank) {
        IE_THROW() << errorPrefix << " should have same rank for input and output tensor";
    } else {
        for (size_t r = 0; r < srcRank; r++) {
            if (!dimsEqualWeak(srcDataDim[r], dstDataDim[r])) {
                IE_THROW() << errorPrefix << " should have same shape for input and output tensor. The input shape is "
                           << srcDataDim[r] << ", while output shape is " << dstDataDim[r] << " for " << r << "th dimension";
            }
        }
    }
    // specific check
    switch (scatterUpdateMode) {
        case ScatterUpdateMode::ScatterUpdate: {
            if (updateRank != (srcRank + indicesRank - 1)) {
                IE_THROW() << errorPrefix << " do not have matched tensor rank relationship for input, indices and update";
            }
            break;
        }
        case ScatterUpdateMode::ScatterNDUpdate: {
            if (indicesDim[indicesRank - 1] != Shape::UNDEFINED_DIM) {
                size_t k = indicesDim[indicesRank - 1];
                if (k > srcRank) {
                    IE_THROW() << errorPrefix << "' do not have an correct indices' last dimension value, "
                        << "which should be smaller than or equal to input tensor rank";
                }

                size_t tupleRank = indicesRank - 1;
                SizeVector expectUpdateShape(tupleRank + srcRank - k, 0);
                int updateAxisIter = 0;
                for (size_t ri = 0; ri < tupleRank; ri++) {
                    expectUpdateShape[updateAxisIter] = indicesDim[ri];
                    updateAxisIter++;
                }
                for (size_t rd = k; rd < srcRank; rd++) {
                    expectUpdateShape[updateAxisIter] = srcDataDim[rd];
                    updateAxisIter++;
                }
                if (expectUpdateShape.size() != updateRank) {
                    IE_THROW() << errorPrefix << " do not have matched tensor rank relationship for input, indices and update";
                }
                for (size_t ru = 0; ru < updateRank; ru++) {
                    if (!dimsEqualWeak(updateDim[ru], expectUpdateShape[ru])) {
                        IE_THROW() << errorPrefix << " do not have matched tensor shape relationship for input, indices and update";
                    }
                }
            }
            break;
        }
        case ScatterUpdateMode::ScatterElementsUpdate: {
            if (srcRank != indicesRank || srcRank != updateRank) {
                IE_THROW() << errorPrefix << " do not have the same tensor rank for input, indices and update";
            }
            for (size_t ri = 0; ri < indicesRank; ri++) {
                if (!dimsEqualWeak(indicesDim[ri], updateDim[ri])) {
                    IE_THROW() << errorPrefix << " do not have the same tensor shape for indices and update";
                }
            }
            break;
        }
        default: {
            IE_THROW() << errorPrefix << " is not supported";
        }
    }

    indicesPrec = getOriginalInputPrecisionAtPort(INDICES_ID);
    auto indicesType = MKLDNNExtensionUtils::IEPrecisionToDataType(indicesPrec);
    indicesSize = MKLDNNExtensionUtils::sizeOfDataType(indicesType);
    if (indicesSize >= 8) {
        indicesPrec = Precision::I64;
        indicesSize = 8;
    } else {
        indicesPrec = Precision::I32;
        indicesSize = 4;
    }

    if (axisRelaxed) {
        axisPrec = getOriginalInputPrecisionAtPort(AXIS_ID);
        auto axisType = MKLDNNExtensionUtils::IEPrecisionToDataType(axisPrec);
        axisSize = MKLDNNExtensionUtils::sizeOfDataType(axisType);
        if (axisSize >= 8) {
            axisPrec = Precision::I64;
            axisSize = 8;
        } else {
            axisPrec = Precision::I32;
            axisSize = 4;
        }
    }

    dataPrec = getOriginalInputPrecisionAtPort(DATA_ID);
    dataSize = dataPrec.size();

    bool canBeInplace = !isDynamicNode() && getParentEdgeAt(DATA_ID)->getParent()->getChildEdges().size() == 1 &&
                        !getParentEdgeAt(DATA_ID)->getParent()->isConstant();

    NodeConfig config;
    config.dynBatchSupport = false;
    if (axisRelaxed) {
        config.inConfs.resize(4);
    } else {
        config.inConfs.resize(3);
    }
    config.outConfs.resize(1);
    config.inConfs[DATA_ID].constant(false);
    config.inConfs[INDICES_ID].constant(false);
    config.inConfs[UPDATE_ID].constant(false);
    config.outConfs[0].constant(false);
    config.inConfs[DATA_ID].inPlace(canBeInplace ? 0 : -1);
    config.inConfs[INDICES_ID].inPlace(-1);
    config.inConfs[UPDATE_ID].inPlace(-1);
    config.outConfs[0].inPlace(canBeInplace ? 0 : -1);
    if (axisRelaxed) {
        config.inConfs[AXIS_ID].constant(false);
        config.inConfs[AXIS_ID].inPlace(-1);
    }

    std::vector<PortConfigurator> inPortConfig{{LayoutType::ncsp, dataPrec}, {LayoutType::ncsp, indicesPrec}, {LayoutType::ncsp, dataPrec}};
    if (axisRelaxed)
        inPortConfig.emplace_back(LayoutType::ncsp, axisPrec);
    addSupportedPrimDesc(inPortConfig,
                         {{LayoutType::ncsp, dataPrec}},
                          impl_desc_type::unknown);
}

bool MKLDNNScatterUpdateNode::needPrepareParams() const {
    return false;
}

void MKLDNNScatterUpdateNode::executeDynamicImpl(mkldnn::stream strm) {
    execute(strm);
}

int64_t MKLDNNScatterUpdateNode::getIndicesValue(uint8_t *indices, size_t offset) {
    auto *indicesPtr = indices + offset * indicesSize;
    int64_t ret = 0;
    if (indicesSize == 4) {
        auto *indicesPtr32 = reinterpret_cast<int32_t*>(indicesPtr);
        ret = *indicesPtr32;
    } else {
        auto *indicesPtr64 = reinterpret_cast<int64_t*>(indicesPtr);
        ret = *indicesPtr64;
    }
    return ret;
}

// 5D example:
// shapeND: n     c     d     h    w
// blockND: ncdhw cdhw  dhw   hw   w    1
// index  : 0      1    2     3    4    5
static std::vector<size_t> getBlockND(const VectorDims& shape) {
    size_t shapeRank = shape.size();
    std::vector<size_t> blockND(shapeRank + 1, 1);
    for (int i = shapeRank - 1; i >= 0; i--) {
        blockND[i] = shape[i] * blockND[i+1];
    }
    return blockND;
}

void MKLDNNScatterUpdateNode::execute(mkldnn::stream strm) {
    auto &srcMemPtr = getParentEdgeAt(DATA_ID)->getMemoryPtr();
    auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto &indicesMemPtr = getParentEdgeAt(INDICES_ID)->getMemoryPtr();
    auto &updateMemPtr = getParentEdgeAt(UPDATE_ID)->getMemoryPtr();

    uint8_t *dstPtr = reinterpret_cast<uint8_t*>(dstMemPtr->GetPtr());
    uint8_t *srcPtr = reinterpret_cast<uint8_t*>(srcMemPtr->GetPtr());
    uint8_t *indicesPtr = reinterpret_cast<uint8_t*>(indicesMemPtr->GetPtr());
    uint8_t *updatePtr = reinterpret_cast<uint8_t*>(updateMemPtr->GetPtr());

    const auto& srcDataDim = getParentEdgeAt(DATA_ID)->getMemory().getStaticDims();
    const auto& indicesDim = getParentEdgeAt(INDICES_ID)->getMemory().getStaticDims();
    size_t srcRank = srcDataDim.size();
    int axis = 0;
    if (axisRelaxed) {
        auto &axisMemPtr = getParentEdgeAt(AXIS_ID)->getMemoryPtr();
        uint8_t *axisPtr = reinterpret_cast<uint8_t*>(axisMemPtr->GetPtr());
        if (axisSize == 4) {
            auto *axisPtr32 = reinterpret_cast<int32_t*>(axisPtr);
            axis = *axisPtr32;
        } else {
            auto *axisPtr64 = reinterpret_cast<int64_t*>(axisPtr);
            axis = *axisPtr64;
        }

        if (axis >= static_cast<int>(srcRank) || axis < (static_cast<int>(srcRank) * - 1)) {
            IE_THROW() << errorPrefix
            << " should have axis value in range [-r, r - 1], where r is the rank of input data";
        }
        axis = axis < 0 ? (axis + srcRank) : axis;

        size_t srcDimAxis = srcDataDim[axis];
        std::vector<size_t> indicesBlockND = getBlockND(indicesDim);
        parallel_nt(0, [&](const int ithr, const int nthr) {
            size_t start = 0, end = 0;
            splitter(indicesBlockND[0], nthr, ithr, start, end);
            for (int i = start; i < end; i++) {
                int64_t idxValue =  getIndicesValue(indicesPtr, i);
                if (idxValue >= static_cast<int64_t>(srcDimAxis) || idxValue < 0) {
                    IE_THROW() << errorPrefix
                    << " have indices value that points to non-existing output tensor element";
                }
            }
        });

        if (scatterUpdateMode == ScatterUpdateMode::ScatterUpdate) {
            SizeVector indicesDim = getParentEdgeAt(INDICES_ID)->getMemory().getStaticDims();
            SizeVector updateDim = getParentEdgeAt(UPDATE_ID)->getMemory().getStaticDims();
            size_t indicesRank = indicesDim.size();
            size_t updateRank = updateDim.size();
            SizeVector expectUpdateShape(srcRank + indicesRank - 1, 0);
            int axisIter = 0;
            for (size_t rs = 0; rs < srcRank; rs++) {
                if (rs != axis) {
                    expectUpdateShape[axisIter] = srcDataDim[rs];
                    axisIter++;
                } else {
                    for (size_t ri = 0; ri < indicesRank; ri++) {
                        expectUpdateShape[axisIter] = indicesDim[ri];
                        axisIter++;
                    }
                }
            }
            if (updateRank > expectUpdateShape.size())
                IE_THROW() << errorPrefix << " cannot update shape. New rank: "
                    << updateRank << ", expected: " << expectUpdateShape.size();
            for (size_t ru = 0; ru < updateRank; ru++) {
                if (updateDim[ru] != expectUpdateShape[ru]) {
                    IE_THROW() << errorPrefix
                    << " do not have matched tensor shape relationship for input, indices and update";
                }
            }
        }
    }

    if (srcPtr != dstPtr) {
        std::vector<size_t> srcBlockND = getBlockND(srcDataDim);
        parallel_nt(0, [&](const int ithr, const int nthr) {
            size_t start = 0, end = 0;
            splitter(srcBlockND[0], nthr, ithr, start, end);
            size_t size = (end - start) * dataSize;
            start *= dataSize;
            cpu_memcpy(dstPtr + start, srcPtr + start, size);
        });
    }

    if (isInputTensorAtPortEmpty(INDICES_ID)) {
        return;
    }

    switch (scatterUpdateMode) {
        case ScatterUpdateMode::ScatterUpdate: {
            scatterUpdate(indicesPtr, updatePtr, axis, dstPtr);
            break;
        }
        case ScatterUpdateMode::ScatterNDUpdate: {
            scatterNDUpdate(indicesPtr, updatePtr, dstPtr);
            break;
        }
        case ScatterUpdateMode::ScatterElementsUpdate: {
            scatterElementsUpdate(indicesPtr, updatePtr, axis, dstPtr);
            break;
        }
        default: {
            IE_THROW() << errorPrefix
            << " is not supported";
        }
    }
}

// For the data tensor of shape [d_0, d_1, ..., d_n],
// and indices tensor of shape [i_0, i_1, ..., i_k].
// Updates tensor shape should be [d_0, d_1, ... d_(axis - 1), i_0, i_1, ..., i_k, d_(axis + 1), ..., d_n].
void MKLDNNScatterUpdateNode::scatterUpdate(uint8_t *indices, uint8_t *update, int axis, uint8_t *dstData) {
    const auto& srcDataDim = getParentEdgeAt(DATA_ID)->getMemory().getStaticDims();
    const auto& indicesDim = getParentEdgeAt(INDICES_ID)->getMemory().getStaticDims();
    const auto& updateDim = getParentEdgeAt(UPDATE_ID)->getMemory().getStaticDims();
    size_t indicesRank = indicesDim.size();

    std::vector<size_t> srcBlockND = getBlockND(srcDataDim);
    std::vector<size_t> updateBlockND = getBlockND(updateDim);

    const size_t mulIdentity = 1;
    size_t idxLength = mulIdentity;
    for (size_t ri = 0; ri < indicesRank; ri++) {
        idxLength *= indicesDim[ri];
    }
    size_t batchToUpdate = mulIdentity;
    for (size_t x = 0; x < axis; x++) {
        batchToUpdate *= srcDataDim[x];
    }
    // blockToUpdate is srcBlockND[axis + 1], also is updateBlockND[axis + indicesRank]
    size_t blockToUpdate = srcBlockND[axis + 1];
    size_t blockToUpdateSize = blockToUpdate * dataSize;

    parallel_for2d(batchToUpdate, idxLength, [&](size_t b, size_t idx) {
        int64_t idxValue = getIndicesValue(indices, idx);
        uint8_t *dstEntry = dstData + (b * srcBlockND[axis] + idxValue * blockToUpdate) * dataSize;
        uint8_t *updateEntry = update + (b * updateBlockND[axis] + idx * blockToUpdate) * dataSize;
        cpu_memcpy(dstEntry, updateEntry, blockToUpdateSize);
    });
}

// indices is a (q-1)-dimension tensor of k-tuple,
// k is indices.shape[-1] and should not be greater than rank of input, q is rank of indicies.
// updates is a (q-1)-dimension tensor of replacement-slice-values
void MKLDNNScatterUpdateNode::scatterNDUpdate(uint8_t *indices, uint8_t *update, uint8_t *dstData) {
    const auto& srcDataDim = getParentEdgeAt(DATA_ID)->getMemory().getStaticDims();
    const auto& indicesDim = getParentEdgeAt(INDICES_ID)->getMemory().getStaticDims();
    size_t indicesRank = indicesDim.size();

    std::vector<size_t> srcBlockND = getBlockND(srcDataDim);

    size_t k = indicesDim[indicesRank - 1];
    size_t idxTupleNum = 1;
    for (size_t ri = 0; ri < indicesRank - 1; ri++) {
        idxTupleNum *= indicesDim[ri];
    }

    size_t sizeToUpdate = srcBlockND[k] * dataSize;
    parallel_for(idxTupleNum, [&](size_t tupleIdx) {
        size_t indicesOffset = tupleIdx * k;
        size_t dstOffset = 0;
        for (int i = 0; i < k; i++) {
            size_t idxValue = getIndicesValue(indices, indicesOffset + i);
            dstOffset += idxValue * srcBlockND[i + 1];
        }
        dstOffset *= dataSize;
        size_t updateOffset = tupleIdx * sizeToUpdate;
        cpu_memcpy(dstData + dstOffset, update + updateOffset, sizeToUpdate);
    });
}

// output[indices[i][j][k]][j][k] = updates[i][j][k] if axis = 0,
// output[i][indices[i][j][k]][k] = updates[i][j][k] if axis = 1,
// output[i][j][indices[i][j][k]] = updates[i][j][k] if axis = 2.
void MKLDNNScatterUpdateNode::scatterElementsUpdate(uint8_t *indices, uint8_t *update, int axis, uint8_t *dstData) {
    const auto& srcDataDim = getParentEdgeAt(DATA_ID)->getMemory().getStaticDims();
    const auto& updateDim = getParentEdgeAt(UPDATE_ID)->getMemory().getStaticDims();
    size_t updateRank = updateDim.size();

    std::vector<size_t> srcBlockND = getBlockND(srcDataDim);
    std::vector<size_t> updateBlockND = getBlockND(updateDim);

    parallel_nt(0, [&](const int ithr, const int nthr) {
        int j;
        size_t i, dst_idx = 0, start = 0, end = 0;
        SizeVector tensorItr(updateRank, 0);
        splitter(updateBlockND[0], nthr, ithr, start, end);
        for (j = updateRank - 1, i = start; j >= 0; j--) {
            tensorItr[j] = i % updateDim[j];
            i /= updateDim[j];
        }

        for (i = 0; i < static_cast<size_t>(axis); ++i)
            dst_idx += tensorItr[i] * srcBlockND[i + 1];
        for (i++; i < updateRank; ++i)
            dst_idx += tensorItr[i] * srcBlockND[i + 1];

        for (size_t iwork = start; iwork < end; iwork++) {
            int64_t idxValue = getIndicesValue(indices, iwork);
            if (idxValue < srcDataDim[axis])
                cpu_memcpy(dstData + dataSize * (dst_idx + idxValue * srcBlockND[axis + 1]),
                            update + iwork * dataSize, dataSize);

            for (j = updateRank - 1; j >= 0; j--) {
                tensorItr[j]++;
                if (tensorItr[j] < updateDim[j]) {
                    if (j != static_cast<size_t>(axis))
                        dst_idx += srcBlockND[j + 1];
                    break;
                } else {
                    tensorItr[j] = 0;
                    for (dst_idx = 0, i = 0; i < static_cast<size_t>(axis); ++i)
                        dst_idx += tensorItr[i] * srcBlockND[i + 1];
                    for (i++; i < updateRank; ++i)
                        dst_idx += tensorItr[i] * srcBlockND[i + 1];
                }
            }
        }
    });
}

bool MKLDNNScatterUpdateNode::created() const {
    return getType() == ScatterUpdate || getType() == ScatterElementsUpdate || getType() == ScatterNDUpdate;
}

REG_MKLDNN_PRIM_FOR(MKLDNNScatterUpdateNode, ScatterUpdate);
REG_MKLDNN_PRIM_FOR(MKLDNNScatterUpdateNode, ScatterElementsUpdate);
REG_MKLDNN_PRIM_FOR(MKLDNNScatterUpdateNode, ScatterNDUpdate);
