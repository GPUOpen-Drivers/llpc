/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcSystemValues.cpp
 * @brief LLPC source file: per-shader per-pass generating and cache of shader system pointers
 ***********************************************************************************************************************
 */
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"

#include "SPIRVInternal.h"
#include "llpcPipelineState.h"
#include "llpcSystemValues.h"
#include "llpcTargetInfo.h"

#define DEBUG_TYPE "llpc-system-values"

using namespace Llpc;
using namespace llvm;

namespace llvm
{

namespace cl
{

extern opt<uint32_t> ShadowDescTablePtrHigh;

} // cl

} // llvm

// =====================================================================================================================
// Initialize this ShaderSystemValues if it was previously uninitialized.
void ShaderSystemValues::Initialize(
    PipelineState*  pPipelineState, // [in] Pipeline state
    Function*       pEntryPoint)    // [in] Shader entrypoint
{
    if (m_pEntryPoint == nullptr)
    {
        m_pEntryPoint = pEntryPoint;
        m_shaderStage = GetShaderStageFromFunction(pEntryPoint);
        m_pContext = &pEntryPoint->getParent()->getContext();
        m_pPipelineState = pPipelineState;

        LLPC_ASSERT(m_shaderStage != ShaderStageInvalid);
        LLPC_ASSERT(m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs.initialized);
    }
}

// =====================================================================================================================
// Get ES-GS ring buffer descriptor (for VS/TES output or GS input)
Value* ShaderSystemValues::GetEsGsRingBufDesc()
{
    if (m_pEsGsRingBufDesc == nullptr)
    {
        uint32_t tableOffset = 0;
        switch (m_shaderStage)
        {
        case ShaderStageVertex:
        case ShaderStageTessEval:
            tableOffset = SI_DRV_TABLE_ES_RING_OUT_OFFS;
            break;
        case ShaderStageGeometry:
            tableOffset = SI_DRV_TABLE_GS_RING_IN_OFFS;
            break;
        default:
            LLPC_NEVER_CALLED();
            break;
        }

        IRBuilder<> builder(&*m_pEntryPoint->front().getFirstInsertionPt());
        m_pEsGsRingBufDesc = LoadDescFromDriverTable(tableOffset, builder);
        if ((m_shaderStage != ShaderStageGeometry) && (m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major >= 8))
        {
            // NOTE: For GFX8+, we have to explicitly set DATA_FORMAT for GS-VS ring buffer descriptor for
            // VS/TES output.
            m_pEsGsRingBufDesc = SetRingBufferDataFormat(m_pEsGsRingBufDesc, BUF_DATA_FORMAT_32, builder);
        }
    }
    return m_pEsGsRingBufDesc;
}

// =====================================================================================================================
// Get the descriptor for tessellation factor (TF) buffer (TCS output)
Value* ShaderSystemValues::GetTessFactorBufDesc()
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
    if (m_pTfBufDesc == nullptr)
    {
        IRBuilder<> builder(&*m_pEntryPoint->front().getFirstInsertionPt());
        m_pTfBufDesc = LoadDescFromDriverTable(SI_DRV_TABLE_TF_BUFFER_OFFS, builder);
    }
    return m_pTfBufDesc;
}

// =====================================================================================================================
// Extract value of primitive ID (TCS)
Value* ShaderSystemValues::GetPrimitiveId()
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
    if (m_pPrimitiveId == nullptr)
    {
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
        m_pPrimitiveId = GetFunctionArgument(m_pEntryPoint, pIntfData->entryArgIdxs.tcs.patchId, "patchId");
    }
    return m_pPrimitiveId;
}

// =====================================================================================================================
// Get invocation ID (TCS)
Value* ShaderSystemValues::GetInvocationId()
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
    if (m_pInvocationId == nullptr)
    {
        auto pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);

        // invocationId = relPatchId[12:8]
        Value* args[] =
        {
            GetFunctionArgument(m_pEntryPoint, pIntfData->entryArgIdxs.tcs.relPatchId, "relPatchId"),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 8),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 5)
        };
        m_pInvocationId = EmitCall("llvm.amdgcn.ubfe.i32",
                                   Type::getInt32Ty(*m_pContext),
                                   args,
                                   Attribute::ReadNone,
                                   pInsertPos);
    }
    return m_pInvocationId;
}

// =====================================================================================================================
// Get relative patchId (TCS)
Value* ShaderSystemValues::GetRelativeId()
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
    if (m_pRelativeId == nullptr)
    {
        auto pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
        auto pRelPatchId = GetFunctionArgument(m_pEntryPoint, pIntfData->entryArgIdxs.tcs.relPatchId, "relPatchId");

        // relativeId = relPatchId[7:0]
        m_pRelativeId = BinaryOperator::CreateAnd(pRelPatchId,
                                                  ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xFF),
                                                  "",
                                                  pInsertPos);
    }
    return m_pRelativeId;
}

// =====================================================================================================================
// Get offchip LDS descriptor (TCS and TES)
Value* ShaderSystemValues::GetOffChipLdsDesc()
{
    LLPC_ASSERT((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval));
    if (m_pOffChipLdsDesc == nullptr)
    {
        IRBuilder<> builder(&*m_pEntryPoint->front().getFirstInsertionPt());
        m_pOffChipLdsDesc = LoadDescFromDriverTable(SI_DRV_TABLE_HS_BUFFER0_OFFS, builder);
    }
    return m_pOffChipLdsDesc;
}

// =====================================================================================================================
// Get tessellated coordinate (TES)
Value* ShaderSystemValues::GetTessCoord()
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessEval);
    if (m_pTessCoord == nullptr)
    {
        auto pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);

        Value* pTessCoordX = GetFunctionArgument(m_pEntryPoint, pIntfData->entryArgIdxs.tes.tessCoordX, "tessCoordX");
        Value* pTessCoordY = GetFunctionArgument(m_pEntryPoint, pIntfData->entryArgIdxs.tes.tessCoordY, "tessCoordY");
        Value* pTessCoordZ = BinaryOperator::CreateFAdd(pTessCoordX, pTessCoordY, "", pInsertPos);

        pTessCoordZ = BinaryOperator::CreateFSub(ConstantFP::get(Type::getFloatTy(*m_pContext), 1.0f),
                                                 pTessCoordZ,
                                                 "",
                                                 pInsertPos);

        auto primitiveMode = m_pPipelineState->GetShaderModes()->GetTessellationMode().primitiveMode;
        pTessCoordZ = (primitiveMode == PrimitiveMode::Triangles) ?
                          pTessCoordZ : ConstantFP::get(Type::getFloatTy(*m_pContext), 0.0f);

        m_pTessCoord = UndefValue::get(VectorType::get(Type::getFloatTy(*m_pContext), 3));
        m_pTessCoord = InsertElementInst::Create(m_pTessCoord,
                                                 pTessCoordX,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                 "",
                                                 pInsertPos);
        m_pTessCoord = InsertElementInst::Create(m_pTessCoord,
                                                 pTessCoordY,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                 "",
                                                 pInsertPos);
        m_pTessCoord = InsertElementInst::Create(m_pTessCoord,
                                                 pTessCoordZ,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                 "",
                                                 pInsertPos);
    }
    return m_pTessCoord;
}

// =====================================================================================================================
// Get ES -> GS offsets (GS in)
Value* ShaderSystemValues::GetEsGsOffsets()
{
    LLPC_ASSERT(m_shaderStage == ShaderStageGeometry);
    if (m_pEsGsOffsets == nullptr)
    {
        auto pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);

        m_pEsGsOffsets = UndefValue::get(VectorType::get(Type::getInt32Ty(*m_pContext), 6));
        for (uint32_t i = 0; i < InterfaceData::MaxEsGsOffsetCount; ++i)
        {
            auto pEsGsOffset = GetFunctionArgument(m_pEntryPoint,
                                                   pIntfData->entryArgIdxs.gs.esGsOffsets[i],
                                                   Twine("esGsOffset") + Twine(i));
            m_pEsGsOffsets = InsertElementInst::Create(m_pEsGsOffsets,
                                                       pEsGsOffset,
                                                       ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                       "",
                                                       pInsertPos);
        }
    }
    return m_pEsGsOffsets;
}

// =====================================================================================================================
// Get GS -> VS ring buffer descriptor (GS out and copy shader in)
Value* ShaderSystemValues::GetGsVsRingBufDesc(
    uint32_t streamId)  // Stream ID, always 0 for copy shader
{
    LLPC_ASSERT((m_shaderStage == ShaderStageGeometry) || (m_shaderStage == ShaderStageCopyShader));
    if (m_gsVsRingBufDescs.size() <= streamId)
    {
        m_gsVsRingBufDescs.resize(streamId + 1);
    }
    if (m_gsVsRingBufDescs[streamId] == nullptr)
    {
        IRBuilder<> builder(&*m_pEntryPoint->front().getFirstInsertionPt());

        if (m_shaderStage == ShaderStageGeometry)
        {
            const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage);

            // Geometry shader, using GS-VS ring for output.
            Value* pDesc = LoadDescFromDriverTable(SI_DRV_TABLE_GS_RING_OUT0_OFFS + streamId, builder);

            uint32_t outLocStart = 0;
            for (int i = 0; i < streamId; ++i)
                outLocStart += pResUsage->inOutUsage.gs.outLocCount[i];

            // streamSize[streamId] = outLocCount[streamId] * 4 * sizeof(uint32_t)
            // streamOffset = (streamSize[0] + ... + streamSize[streamId - 1]) * 64 * outputVertices
            uint32_t baseAddr = outLocStart *
                                m_pPipelineState->GetShaderModes()->GetGeometryShaderMode().outputVertices
                * sizeof(uint32_t) * 4 * 64;

            // Patch GS-VS ring buffer descriptor base address for GS output
            Value* pGsVsOutRingBufDescElem0 = builder.CreateExtractElement(pDesc, (uint64_t)0);
            pGsVsOutRingBufDescElem0 = builder.CreateAdd(pGsVsOutRingBufDescElem0, builder.getInt32(baseAddr));
            pDesc = builder.CreateInsertElement(pDesc, pGsVsOutRingBufDescElem0, (uint64_t)0);

            // Patch GS-VS ring buffer descriptor stride for GS output
            Value* pGsVsRingBufDescElem1 = builder.CreateExtractElement(pDesc, (uint64_t)1);

            // Clear stride in SRD DWORD1
            SqBufRsrcWord1 strideClearMask = {};
            strideClearMask.u32All         = UINT32_MAX;
            strideClearMask.bits.STRIDE    = 0;
            pGsVsRingBufDescElem1 = builder.CreateAnd(pGsVsRingBufDescElem1, builder.getInt32(strideClearMask.u32All));

            // Calculate and set stride in SRD dword1
            uint32_t gsVsStride = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode().outputVertices *
                                  pResUsage->inOutUsage.gs.outLocCount[streamId] *
                                  sizeof(uint32_t) * 4;

            SqBufRsrcWord1 strideSetValue = {};
            strideSetValue.bits.STRIDE = gsVsStride;
            pGsVsRingBufDescElem1 = builder.CreateOr(pGsVsRingBufDescElem1, builder.getInt32(strideSetValue.u32All));

            pDesc = builder.CreateInsertElement(pDesc, pGsVsRingBufDescElem1, (uint64_t)1);

            if (m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major >= 8)
            {
                // NOTE: For GFX8+, we have to explicitly set DATA_FORMAT for GS-VS ring buffer descriptor.
                pDesc = SetRingBufferDataFormat(pDesc, BUF_DATA_FORMAT_32, builder);
            }

            m_gsVsRingBufDescs[streamId] = pDesc;
        }
        else
        {
            // Copy shader, using GS-VS ring for input.
            LLPC_ASSERT(streamId == 0);
            m_gsVsRingBufDescs[streamId] = LoadDescFromDriverTable(SI_DRV_TABLE_VS_RING_IN_OFFS, builder);
        }
    }
    return m_gsVsRingBufDescs[streamId];
}

// =====================================================================================================================
// Get pointers to emit counters (GS)
ArrayRef<Value*> ShaderSystemValues::GetEmitCounterPtr()
{
    LLPC_ASSERT(m_shaderStage == ShaderStageGeometry);
    if (m_emitCounterPtrs.empty())
    {
        // TODO: We should only insert those offsets required by the specified input primitive.

        // Setup GS emit vertex counter
        auto& dataLayout = m_pEntryPoint->getParent()->getDataLayout();
        auto pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
        for (int i = 0; i < MaxGsStreams; ++i)
        {
            auto pEmitCounterPtr = new AllocaInst(Type::getInt32Ty(*m_pContext),
                                                  dataLayout.getAllocaAddrSpace(),
                                                  "",
                                                  pInsertPos);
            new StoreInst(ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                          pEmitCounterPtr,
                          pInsertPos);
            m_emitCounterPtrs.push_back(pEmitCounterPtr);
        }
    }
    return m_emitCounterPtrs;
}

// =====================================================================================================================
// Get descriptor table pointer
Value* ShaderSystemValues::GetDescTablePtr(
    uint32_t        descSet)        // Descriptor set ID
{
    if (m_descTablePtrs.size() <= descSet)
    {
        m_descTablePtrs.resize(descSet + 1);
    }
    if (m_descTablePtrs[descSet] == nullptr)
    {
        // Find the node.
        uint32_t resNodeIdx = FindResourceNodeByDescSet(descSet);
        if (resNodeIdx != InvalidValue)
        {
            // Get the 64-bit extended node value.
            auto pDescTablePtrTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_pContext), UINT32_MAX),
                                                    ADDR_SPACE_CONST);
            m_descTablePtrs[descSet] = GetExtendedResourceNodeValue(resNodeIdx,
                                                                    pDescTablePtrTy,
                                                                    InvalidValue);
        }
    }
    return m_descTablePtrs[descSet];
}

// =====================================================================================================================
// Get shadow descriptor table pointer
Value* ShaderSystemValues::GetShadowDescTablePtr(
    uint32_t        descSet)        // Descriptor set ID
{
    if (m_shadowDescTablePtrs.size() <= descSet)
    {
        m_shadowDescTablePtrs.resize(descSet + 1);
    }
    if (m_shadowDescTablePtrs[descSet] == nullptr)
    {
        // Find the node.
        uint32_t resNodeIdx = FindResourceNodeByDescSet(descSet);
        if (resNodeIdx != InvalidValue)
        {
            // Get the 64-bit extended node value.
            auto pDescTablePtrTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_pContext), UINT32_MAX),
                                                    ADDR_SPACE_CONST);
            m_shadowDescTablePtrs[descSet] = GetExtendedResourceNodeValue(resNodeIdx,
                                                                          pDescTablePtrTy,
                                                                          cl::ShadowDescTablePtrHigh);
        }
    }
    return m_shadowDescTablePtrs[descSet];
}

// =====================================================================================================================
// Get dynamic descriptor
Value* ShaderSystemValues::GetDynamicDesc(
    uint32_t        dynDescIdx)     // Dynamic descriptor index
{
    if (dynDescIdx >= InterfaceData::MaxDynDescCount)
    {
        return nullptr;
    }
    if (m_dynDescs.size() <= dynDescIdx)
    {
        m_dynDescs.resize(dynDescIdx + 1);
    }
    if (m_dynDescs[dynDescIdx] == nullptr)
    {
        // Find the node.
        uint32_t foundDynDescIdx = 0;
        auto userDataNodes = m_pPipelineState->GetUserDataNodes();
        for (uint32_t i = 0; i != userDataNodes.size(); ++i)
        {
            auto pNode = &userDataNodes[i];
            if  ((pNode->type == ResourceMappingNodeType::DescriptorResource) ||
                 (pNode->type == ResourceMappingNodeType::DescriptorSampler) ||
                 (pNode->type == ResourceMappingNodeType::DescriptorTexelBuffer) ||
                 (pNode->type == ResourceMappingNodeType::DescriptorFmask) ||
                 (pNode->type == ResourceMappingNodeType::DescriptorBuffer) ||
                 (pNode->type == ResourceMappingNodeType::DescriptorBufferCompact))
            {
                if (foundDynDescIdx == dynDescIdx)
                {
                    // Get the node value.
                    m_dynDescs[dynDescIdx] = GetResourceNodeValue(i);
                    break;
                }
                ++foundDynDescIdx;
            }
        }
    }
    return m_dynDescs[dynDescIdx];
}

// =====================================================================================================================
// Get internal global table pointer
Value* ShaderSystemValues::GetInternalGlobalTablePtr()
{
    if (m_pInternalGlobalTablePtr == nullptr)
    {
        auto pPtrTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_pContext), UINT32_MAX), ADDR_SPACE_CONST);
        // Global table is always the first function argument
        m_pInternalGlobalTablePtr = MakePointer(GetFunctionArgument(m_pEntryPoint, 0, "globalTable"),
                                                pPtrTy,
                                                InvalidValue);
    }
    return m_pInternalGlobalTablePtr;
}

// =====================================================================================================================
// Get internal per shader table pointer
Value* ShaderSystemValues::GetInternalPerShaderTablePtr()
{
    if (m_pInternalPerShaderTablePtr == nullptr)
    {
        auto pPtrTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_pContext), UINT32_MAX), ADDR_SPACE_CONST);
        // Per shader table is always the second function argument
        m_pInternalPerShaderTablePtr = MakePointer(GetFunctionArgument(m_pEntryPoint, 1, "perShaderTable"),
                                                   pPtrTy,
                                                   InvalidValue);
    }
    return m_pInternalPerShaderTablePtr;
}

// =====================================================================================================================
// Get number of workgroups value
Value* ShaderSystemValues::GetNumWorkgroups()
{
    if (m_pNumWorkgroups == nullptr)
    {
        Instruction* pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);

        auto pNumWorkgroupPtr = GetFunctionArgument(m_pEntryPoint,
                                                    pIntfData->entryArgIdxs.cs.numWorkgroupsPtr,
                                                    "numWorkgroupsPtr");
        auto pNumWorkgroups = new LoadInst(pNumWorkgroupPtr, "", pInsertPos);
        pNumWorkgroups->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(pInsertPos->getContext(), {}));
        m_pNumWorkgroups = pNumWorkgroups;
    }
    return m_pNumWorkgroups;
}

// =====================================================================================================================
// Get spilled push constant pointer
Value* ShaderSystemValues::GetSpilledPushConstTablePtr()
{
    if (m_pSpilledPushConstTablePtr == nullptr)
    {
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
        LLPC_ASSERT(pIntfData->pushConst.resNodeIdx != InvalidValue);
        LLPC_ASSERT(pIntfData->entryArgIdxs.spillTable != InvalidValue);

        Instruction* pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();

        auto pPushConstNode = &m_pPipelineState->GetUserDataNodes()[pIntfData->pushConst.resNodeIdx];
        uint32_t pushConstOffset = pPushConstNode->offsetInDwords * sizeof(uint32_t);

        auto pSpillTablePtrLow = GetFunctionArgument(m_pEntryPoint, pIntfData->entryArgIdxs.spillTable, "spillTable");
        auto pPushConstOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), pushConstOffset);
        auto pSpilledPushConstTablePtrLow = BinaryOperator::CreateAdd(pSpillTablePtrLow,
                                                                      pPushConstOffset,
                                                                      "",
                                                                      pInsertPos);
        auto pTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_pContext), InterfaceData::MaxSpillTableSize),
                                                 ADDR_SPACE_CONST);
        m_pSpilledPushConstTablePtr = MakePointer(pSpilledPushConstTablePtrLow, pTy, InvalidValue);
    }
    return m_pSpilledPushConstTablePtr;
}

// =====================================================================================================================
// Get vertex buffer table pointer
Value* ShaderSystemValues::GetVertexBufTablePtr()
{
    if (m_pVbTablePtr == nullptr)
    {
        // Find the node.
        auto pVbTableNode = FindResourceNodeByType(ResourceMappingNodeType::IndirectUserDataVaPtr);
        if (pVbTableNode != nullptr)
        {
            // Get the 64-bit extended node value.
            auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
            auto pVbTablePtrLow = GetFunctionArgument(m_pEntryPoint,
                                                      pIntfData->entryArgIdxs.vs.vbTablePtr,
                                                      "vbTablePtr");
            static const uint32_t MaxVertexBufferSize = 0x10000000;
            auto pVbTablePtrTy = PointerType::get(ArrayType::get(VectorType::get(Type::getInt32Ty(*m_pContext), 4),
                                                                 MaxVertexBufferSize),
                                                  ADDR_SPACE_CONST);
            m_pVbTablePtr = MakePointer(pVbTablePtrLow, pVbTablePtrTy, InvalidValue);
        }
    }
    return m_pVbTablePtr;
}

// =====================================================================================================================
// Get stream-out buffer descriptor
Value* ShaderSystemValues::GetStreamOutBufDesc(
    uint32_t        xfbBuffer)      // Transform feedback buffer ID
{
    if (m_streamOutBufDescs.size() <= xfbBuffer)
    {
        m_streamOutBufDescs.resize(xfbBuffer + 1);
    }

    if (m_streamOutBufDescs[xfbBuffer] == nullptr)
    {
        auto pStreamOutTablePtr = GetStreamOutTablePtr();
        auto pInsertPos = pStreamOutTablePtr->getNextNode();

        Value* idxs[] =
        {
            ConstantInt::get(Type::getInt64Ty(*m_pContext), 0),
            ConstantInt::get(Type::getInt64Ty(*m_pContext), xfbBuffer)
        };

        auto pStreamOutBufDescPtr = GetElementPtrInst::Create(nullptr, pStreamOutTablePtr, idxs, "", pInsertPos);
        pStreamOutBufDescPtr->setMetadata(MetaNameUniform, MDNode::get(pStreamOutBufDescPtr->getContext(), {}));

        auto pStreamOutBufDesc = new LoadInst(pStreamOutBufDescPtr, "", pInsertPos);
        pStreamOutBufDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(pStreamOutBufDesc->getContext(), {}));
        pStreamOutBufDesc->setAlignment(MaybeAlign(16));

        m_streamOutBufDescs[xfbBuffer] = pStreamOutBufDesc;
    }
    return m_streamOutBufDescs[xfbBuffer];
}

// =====================================================================================================================
// Get stream-out buffer table pointer
Instruction* ShaderSystemValues::GetStreamOutTablePtr()
{
    LLPC_ASSERT((m_shaderStage == ShaderStageVertex) ||
                (m_shaderStage == ShaderStageTessEval) ||
                (m_shaderStage == ShaderStageCopyShader));

    if (m_pStreamOutTablePtr == nullptr)
    {
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
        uint32_t entryArgIdx = 0;

        if (m_shaderStage != ShaderStageCopyShader)
        {
            // Find the node.
            auto pNode = FindResourceNodeByType(ResourceMappingNodeType::StreamOutTableVaPtr);
            if (pNode != nullptr)
            {
                // Get the SGPR number of the stream-out table pointer.
                switch (m_shaderStage)
                {
                case ShaderStageVertex:
                    entryArgIdx = pIntfData->entryArgIdxs.vs.streamOutData.tablePtr;
                    break;
                case ShaderStageTessEval:
                    entryArgIdx = pIntfData->entryArgIdxs.tes.streamOutData.tablePtr;
                    break;
                case ShaderStageCopyShader:
                    entryArgIdx = pIntfData->userDataUsage.gs.copyShaderStreamOutTable;
                    break;
                default:
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
        }
        else
        {
            // Special case code for the copy shader.
            entryArgIdx = pIntfData->userDataUsage.gs.copyShaderStreamOutTable;
        }

        // Get the 64-bit extended node value.
        auto pStreamOutTablePtrLow = GetFunctionArgument(m_pEntryPoint, entryArgIdx, "streamOutTable");
        auto pStreamOutTablePtrTy = PointerType::get(ArrayType::get(VectorType::get(Type::getInt32Ty(*m_pContext), 4),
            MaxTransformFeedbackBuffers), ADDR_SPACE_CONST);
        m_pStreamOutTablePtr = MakePointer(pStreamOutTablePtrLow, pStreamOutTablePtrTy, InvalidValue);
    }
    return m_pStreamOutTablePtr;
}

// =====================================================================================================================
// Make 64-bit pointer of specified type from 32-bit int, extending with the specified value, or PC if InvalidValue
Instruction* ShaderSystemValues::MakePointer(
    Value*    pLowValue,  // [in] 32-bit int value to extend
    Type*     pPtrTy,     // [in] Type that result pointer needs to be
    uint32_t  highValue)  // Value to use for high part, or InvalidValue to use PC
{
    // Insert extending code after pLowValue if it is an instruction.
    Instruction* pInsertPos = nullptr;
    auto pLowValueInst = dyn_cast<Instruction>(pLowValue);
    if (pLowValueInst != nullptr)
    {
        pInsertPos = pLowValueInst->getNextNode();
    }
    else
    {
        pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
    }

    Value* pExtendedPtrValue = nullptr;
    if (highValue == InvalidValue)
    {
        // Use PC.
        if ((m_pPc == nullptr) || isa<Instruction>(pLowValue))
        {
            // Either
            // 1. there is no existing code to s_getpc and cast it, or
            // 2. there is existing code, but pLowValue is an instruction, so it is more complex to figure
            //    out whether it is before or after pLowValue in the code. We generate new s_getpc code anyway
            //    and rely on subsequent CSE to common it up.
            // Insert the s_getpc code at the start of the function, so a later call into here knows it can
            // reuse this PC if its pLowValue is an arg rather than an instruction.
            auto pPcInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
            Value* pPc = EmitCall("llvm.amdgcn.s.getpc",
                                  Type::getInt64Ty(*m_pContext),
                                  ArrayRef<Value*>(),
                                  NoAttrib,
                                  pPcInsertPos);
            m_pPc = new BitCastInst(pPc, VectorType::get(Type::getInt32Ty(*m_pContext), 2), "", pInsertPos);
        }
        else
        {
            pInsertPos = m_pPc->getNextNode();
        }
        pExtendedPtrValue = m_pPc;
    }
    else
    {
        // Use constant highValue value.
        Constant* elements[] =
        {
            UndefValue::get(pLowValue->getType()),
            ConstantInt::get(pLowValue->getType(), highValue)
        };
        pExtendedPtrValue = ConstantVector::get(elements);
    }
    pExtendedPtrValue = InsertElementInst::Create(pExtendedPtrValue,
                                          pLowValue,
                                          ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                          "",
                                          pInsertPos);
    pExtendedPtrValue = CastInst::Create(Instruction::BitCast,
                                         pExtendedPtrValue,
                                         Type::getInt64Ty(*m_pContext),
                                         "",
                                         pInsertPos);
    return CastInst::Create(Instruction::IntToPtr, pExtendedPtrValue, pPtrTy, "", pInsertPos);
}

// =====================================================================================================================
// Get 64-bit extended resource node value
Value* ShaderSystemValues::GetExtendedResourceNodeValue(
    uint32_t        resNodeIdx,     // Resource node index
    Type*           pResNodeTy,     // [in] Pointer type of result
    uint32_t        highValue)      // Value to use for high part, or InvalidValue to use PC
{
    return MakePointer(GetResourceNodeValue(resNodeIdx), pResNodeTy, highValue);
}

// =====================================================================================================================
// Get 32 bit resource node value
Value* ShaderSystemValues::GetResourceNodeValue(
    uint32_t        resNodeIdx)     // Resource node index
{
    auto pInsertPos = &*m_pEntryPoint->front().getFirstInsertionPt();
    auto pIntfData   = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
    auto pNode = &m_pPipelineState->GetUserDataNodes()[resNodeIdx];
    Value* pResNodeValue = nullptr;

    if ((pNode->type == ResourceMappingNodeType::IndirectUserDataVaPtr) ||
        (pNode->type == ResourceMappingNodeType::StreamOutTableVaPtr))
    {
        // Do nothing
    }
    else if ((resNodeIdx < InterfaceData::MaxDescTableCount) && (pIntfData->entryArgIdxs.resNodeValues[resNodeIdx] > 0))
    {
        // Resource node isn't spilled, load its value from function argument
        pResNodeValue = GetFunctionArgument(m_pEntryPoint,
                                            pIntfData->entryArgIdxs.resNodeValues[resNodeIdx],
                                            Twine("resNode") + Twine(resNodeIdx));
    }
    else if (pNode->type != ResourceMappingNodeType::PushConst)
    {
        // Resource node is spilled, load its value from spill table
        uint32_t byteOffset = pNode->offsetInDwords * sizeof(uint32_t);

        Value* idxs[] =
        {
          ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
          ConstantInt::get(Type::getInt32Ty(*m_pContext), byteOffset)
        };
        auto pSpillTablePtr = GetSpillTablePtr();
        pInsertPos = pSpillTablePtr->getNextNode();
        auto pElemPtr = GetElementPtrInst::CreateInBounds(pSpillTablePtr, idxs, "", pInsertPos);

        Type* pResNodePtrTy = nullptr;

        if  ((pNode->type == ResourceMappingNodeType::DescriptorResource) ||
             (pNode->type == ResourceMappingNodeType::DescriptorSampler) ||
             (pNode->type == ResourceMappingNodeType::DescriptorTexelBuffer) ||
             (pNode->type == ResourceMappingNodeType::DescriptorFmask) ||
             (pNode->type == ResourceMappingNodeType::DescriptorBuffer) ||
             (pNode->type == ResourceMappingNodeType::DescriptorBufferCompact))
        {
            pResNodePtrTy = VectorType::get(Type::getInt32Ty(*m_pContext),
                                            pNode->sizeInDwords)->getPointerTo(ADDR_SPACE_CONST);
        }
        else
        {
            pResNodePtrTy = Type::getInt32Ty(*m_pContext)->getPointerTo(ADDR_SPACE_CONST);
        }

        auto pResNodePtr = BitCastInst::CreatePointerCast(pElemPtr, pResNodePtrTy, "", pInsertPos);
        pResNodePtr->setMetadata(MetaNameUniform, MDNode::get(pResNodePtr->getContext(), {}));

        pResNodeValue = new LoadInst(pResNodePtr, "", pInsertPos);
    }
    LLPC_ASSERT(pResNodeValue != nullptr);
    return pResNodeValue;
}

// =====================================================================================================================
// Get spill table pointer
Instruction* ShaderSystemValues::GetSpillTablePtr()
{
    if (m_pSpillTablePtr == nullptr)
    {
        auto pIntfData   = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
        auto pSpillTablePtrLow = GetFunctionArgument(m_pEntryPoint, pIntfData->entryArgIdxs.spillTable, "spillTable");
        auto pSpillTablePtrTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_pContext),
                                                                InterfaceData::MaxSpillTableSize),
                                                 ADDR_SPACE_CONST);
        m_pSpillTablePtr = MakePointer(pSpillTablePtrLow, pSpillTablePtrTy, InvalidValue);
    }
    return m_pSpillTablePtr;
}

// =====================================================================================================================
// Load descriptor from driver table
Instruction* ShaderSystemValues::LoadDescFromDriverTable(
    uint32_t tableOffset,    // Byte offset in driver table
    IRBuilder<>& builder)    // Builder to use for insertion
{
    Value* args[] =
    {
        builder.getInt32(InternalResourceTable),
        builder.getInt32(tableOffset),
        builder.getInt32(0),
    };
    return EmitCall(LlpcName::DescriptorLoadBuffer,
                    VectorType::get(Type::getInt32Ty(*m_pContext), 4),
                    args,
                    NoAttrib,
                    builder);
}

// =====================================================================================================================
// Explicitly set the DATA_FORMAT of ring buffer descriptor.
Value* ShaderSystemValues::SetRingBufferDataFormat(
    Value*          pBufDesc,       // [in] Buffer Descriptor
    uint32_t        dataFormat,     // Data format
    IRBuilder<>&    builder         // [in] Builder to use for inserting instructions
    ) const
{
    Value* pElem3 = builder.CreateExtractElement(pBufDesc, (uint64_t)3);

    SqBufRsrcWord3 dataFormatClearMask;
    dataFormatClearMask.u32All = UINT32_MAX;
#if LLPC_BUILD_GFX10
    // TODO: This code needs to be fixed for gfx10; buffer format is handled differently.
#endif
    dataFormatClearMask.gfx6.DATA_FORMAT = 0;
    pElem3 = builder.CreateAnd(pElem3, builder.getInt32(dataFormatClearMask.u32All));

    SqBufRsrcWord3 dataFormatSetValue = {};
    dataFormatSetValue.gfx6.DATA_FORMAT = dataFormat;
    pElem3 = builder.CreateOr(pElem3, builder.getInt32(dataFormatSetValue.u32All));

    return builder.CreateInsertElement(pBufDesc, pElem3, (uint64_t)3);
}

// =====================================================================================================================
// Find resource node by type
const ResourceNode* ShaderSystemValues::FindResourceNodeByType(
    ResourceMappingNodeType type)           // Resource node type to find
{
    auto userDataNodes = m_pPipelineState->GetUserDataNodes();
    for (uint32_t i = 0; i < userDataNodes.size(); ++i)
    {
        auto pNode = &userDataNodes[i];
        if (pNode->type == type)
        {
            return pNode;
        }
    }
    return nullptr;
}

// =====================================================================================================================
// Find resource node by descriptor set ID
uint32_t ShaderSystemValues::FindResourceNodeByDescSet(
    uint32_t        descSet)        // Descriptor set to find
{
    auto userDataNodes = m_pPipelineState->GetUserDataNodes();
    for (uint32_t i = 0; i < userDataNodes.size(); ++i)
    {
        auto pNode = &userDataNodes[i];
        if ((pNode->type == ResourceMappingNodeType::DescriptorTableVaPtr) &&
              (pNode->innerTable[0].set == descSet))
        {
            return i;
        }
    }
    return InvalidValue;
}

