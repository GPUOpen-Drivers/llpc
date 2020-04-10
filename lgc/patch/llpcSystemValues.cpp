/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "llpcPipelineState.h"
#include "llpcSystemValues.h"
#include "llpcTargetInfo.h"

#define DEBUG_TYPE "llpc-system-values"

using namespace lgc;
using namespace llvm;

// -enable-shadow-desc: enable shadow descriptor table
static cl::opt<bool> EnableShadowDescriptorTable("enable-shadow-desc",
                                                 cl::desc("Enable shadow descriptor table"));
// -shadow-desc-table-ptr-high: high part of VA for shadow descriptor table pointer
static cl::opt<unsigned> ShadowDescTablePtrHigh("shadow-desc-table-ptr-high",
                                                cl::desc("High part of VA for shadow descriptor table pointer"));

// =====================================================================================================================
// Initialize this ShaderSystemValues if it was previously uninitialized.
void ShaderSystemValues::initialize(
    PipelineState*  pipelineState, // [in] Pipeline state
    Function*       entryPoint)    // [in] Shader entrypoint
{
    if (m_entryPoint == nullptr)
    {
        m_entryPoint = entryPoint;
        m_shaderStage = getShaderStageFromFunction(entryPoint);
        m_context = &entryPoint->getParent()->getContext();
        m_pipelineState = pipelineState;

        assert(m_shaderStage != ShaderStageInvalid);
        assert(m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.initialized);

        // Load shadow descriptor table settings from pipeline options.
        const auto pipelineOptions = &m_pipelineState->getOptions();
        switch (pipelineOptions->shadowDescriptorTableUsage)
        {
        case ShadowDescriptorTableUsage::Auto: // Use defaults defined in class
            break;
        case ShadowDescriptorTableUsage::Enable:
            m_enableShadowDescTable = true;
            m_shadowDescTablePtrHigh = pipelineOptions->shadowDescriptorTablePtrHigh;
            break;
        case ShadowDescriptorTableUsage::Disable:
            m_enableShadowDescTable = false;
            break;
        default:
            llvm_unreachable("Unsupported value of shadowDescriptorTableUsage");
            break;
        }

        // Command line options override pipeline options.
        if (EnableShadowDescriptorTable.getNumOccurrences() > 0)
        {
            m_enableShadowDescTable = EnableShadowDescriptorTable;
        }

        if (ShadowDescTablePtrHigh.getNumOccurrences() > 0)
        {
            m_shadowDescTablePtrHigh = ShadowDescTablePtrHigh;
        }
    }
}

// =====================================================================================================================
// Get ES-GS ring buffer descriptor (for VS/TES output or GS input)
Value* ShaderSystemValues::getEsGsRingBufDesc()
{
    if (m_esGsRingBufDesc == nullptr)
    {
        unsigned tableOffset = 0;
        switch (m_shaderStage)
        {
        case ShaderStageVertex:
        case ShaderStageTessEval:
            tableOffset = SiDrvTableEsRingOutOffs;
            break;
        case ShaderStageGeometry:
            tableOffset = SiDrvTableGsRingInOffs;
            break;
        default:
            llvm_unreachable("Should never be called!");
            break;
        }

        BuilderBase builder(&*m_entryPoint->front().getFirstInsertionPt());
        m_esGsRingBufDesc = loadDescFromDriverTable(tableOffset, builder);
        if ((m_shaderStage != ShaderStageGeometry) && (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 8))
        {
            // NOTE: For GFX8+, we have to explicitly set DATA_FORMAT for GS-VS ring buffer descriptor for
            // VS/TES output.
            m_esGsRingBufDesc = setRingBufferDataFormat(m_esGsRingBufDesc, BUF_DATA_FORMAT_32, builder);
        }
    }
    return m_esGsRingBufDesc;
}

// =====================================================================================================================
// Get the descriptor for tessellation factor (TF) buffer (TCS output)
Value* ShaderSystemValues::getTessFactorBufDesc()
{
    assert(m_shaderStage == ShaderStageTessControl);
    if (m_tfBufDesc == nullptr)
    {
        BuilderBase builder(&*m_entryPoint->front().getFirstInsertionPt());
        m_tfBufDesc = loadDescFromDriverTable(SiDrvTableTfBufferOffs, builder);
    }
    return m_tfBufDesc;
}

// =====================================================================================================================
// Extract value of primitive ID (TCS)
Value* ShaderSystemValues::getPrimitiveId()
{
    assert(m_shaderStage == ShaderStageTessControl);
    if (m_primitiveId == nullptr)
    {
        auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
        m_primitiveId = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tcs.patchId, "patchId");
    }
    return m_primitiveId;
}

// =====================================================================================================================
// Get invocation ID (TCS)
Value* ShaderSystemValues::getInvocationId()
{
    assert(m_shaderStage == ShaderStageTessControl);
    if (m_invocationId == nullptr)
    {
        auto insertPos = &*m_entryPoint->front().getFirstInsertionPt();
        auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);

        // invocationId = relPatchId[12:8]
        Value* args[] =
        {
            getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tcs.relPatchId, "relPatchId"),
            ConstantInt::get(Type::getInt32Ty(*m_context), 8),
            ConstantInt::get(Type::getInt32Ty(*m_context), 5)
        };
        m_invocationId = emitCall("llvm.amdgcn.ubfe.i32",
                                   Type::getInt32Ty(*m_context),
                                   args,
                                   Attribute::ReadNone,
                                   insertPos);
    }
    return m_invocationId;
}

// =====================================================================================================================
// Get relative patchId (TCS)
Value* ShaderSystemValues::getRelativeId()
{
    assert(m_shaderStage == ShaderStageTessControl);
    if (m_relativeId == nullptr)
    {
        auto insertPos = &*m_entryPoint->front().getFirstInsertionPt();
        auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
        auto relPatchId = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tcs.relPatchId, "relPatchId");

        // relativeId = relPatchId[7:0]
        m_relativeId = BinaryOperator::CreateAnd(relPatchId,
                                                  ConstantInt::get(Type::getInt32Ty(*m_context), 0xFF),
                                                  "",
                                                  insertPos);
    }
    return m_relativeId;
}

// =====================================================================================================================
// Get offchip LDS descriptor (TCS and TES)
Value* ShaderSystemValues::getOffChipLdsDesc()
{
    assert((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval));
    if (m_offChipLdsDesc == nullptr)
    {
        BuilderBase builder(&*m_entryPoint->front().getFirstInsertionPt());
        m_offChipLdsDesc = loadDescFromDriverTable(SiDrvTableHsBuffeR0Offs, builder);
    }
    return m_offChipLdsDesc;
}

// =====================================================================================================================
// Get tessellated coordinate (TES)
Value* ShaderSystemValues::getTessCoord()
{
    assert(m_shaderStage == ShaderStageTessEval);
    if (m_tessCoord == nullptr)
    {
        auto insertPos = &*m_entryPoint->front().getFirstInsertionPt();
        auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);

        Value* tessCoordX = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tes.tessCoordX, "tessCoordX");
        Value* tessCoordY = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tes.tessCoordY, "tessCoordY");
        Value* tessCoordZ = BinaryOperator::CreateFAdd(tessCoordX, tessCoordY, "", insertPos);

        tessCoordZ = BinaryOperator::CreateFSub(ConstantFP::get(Type::getFloatTy(*m_context), 1.0f),
                                                 tessCoordZ,
                                                 "",
                                                 insertPos);

        auto primitiveMode = m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
        tessCoordZ = (primitiveMode == PrimitiveMode::Triangles) ?
                          tessCoordZ : ConstantFP::get(Type::getFloatTy(*m_context), 0.0f);

        m_tessCoord = UndefValue::get(VectorType::get(Type::getFloatTy(*m_context), 3));
        m_tessCoord = InsertElementInst::Create(m_tessCoord,
                                                 tessCoordX,
                                                 ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                                 "",
                                                 insertPos);
        m_tessCoord = InsertElementInst::Create(m_tessCoord,
                                                 tessCoordY,
                                                 ConstantInt::get(Type::getInt32Ty(*m_context), 1),
                                                 "",
                                                 insertPos);
        m_tessCoord = InsertElementInst::Create(m_tessCoord,
                                                 tessCoordZ,
                                                 ConstantInt::get(Type::getInt32Ty(*m_context), 2),
                                                 "",
                                                 insertPos);
    }
    return m_tessCoord;
}

// =====================================================================================================================
// Get ES -> GS offsets (GS in)
Value* ShaderSystemValues::getEsGsOffsets()
{
    assert(m_shaderStage == ShaderStageGeometry);
    if (m_esGsOffsets == nullptr)
    {
        auto insertPos = &*m_entryPoint->front().getFirstInsertionPt();
        auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);

        m_esGsOffsets = UndefValue::get(VectorType::get(Type::getInt32Ty(*m_context), 6));
        for (unsigned i = 0; i < InterfaceData::MaxEsGsOffsetCount; ++i)
        {
            auto esGsOffset = getFunctionArgument(m_entryPoint,
                                                   intfData->entryArgIdxs.gs.esGsOffsets[i],
                                                   Twine("esGsOffset") + Twine(i));
            m_esGsOffsets = InsertElementInst::Create(m_esGsOffsets,
                                                       esGsOffset,
                                                       ConstantInt::get(Type::getInt32Ty(*m_context), i),
                                                       "",
                                                       insertPos);
        }
    }
    return m_esGsOffsets;
}

// =====================================================================================================================
// Get GS -> VS ring buffer descriptor (GS out and copy shader in)
Value* ShaderSystemValues::getGsVsRingBufDesc(
    unsigned streamId)  // Stream ID, always 0 for copy shader
{
    assert((m_shaderStage == ShaderStageGeometry) || (m_shaderStage == ShaderStageCopyShader));
    if (m_gsVsRingBufDescs.size() <= streamId)
    {
        m_gsVsRingBufDescs.resize(streamId + 1);
    }
    if (m_gsVsRingBufDescs[streamId] == nullptr)
    {
        BuilderBase builder(&*m_entryPoint->front().getFirstInsertionPt());

        if (m_shaderStage == ShaderStageGeometry)
        {
            const auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

            // Geometry shader, using GS-VS ring for output.
            Value* desc = loadDescFromDriverTable(SiDrvTableGsRingOuT0Offs + streamId, builder);

            unsigned outLocStart = 0;
            for (int i = 0; i < streamId; ++i)
                outLocStart += resUsage->inOutUsage.gs.outLocCount[i];

            // streamSize[streamId] = outLocCount[streamId] * 4 * sizeof(unsigned)
            // streamOffset = (streamSize[0] + ... + streamSize[streamId - 1]) * 64 * outputVertices
            unsigned baseAddr = outLocStart *
                                m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices
                * sizeof(unsigned) * 4 * 64;

            // Patch GS-VS ring buffer descriptor base address for GS output
            Value* gsVsOutRingBufDescElem0 = builder.CreateExtractElement(desc, (uint64_t)0);
            gsVsOutRingBufDescElem0 = builder.CreateAdd(gsVsOutRingBufDescElem0, builder.getInt32(baseAddr));
            desc = builder.CreateInsertElement(desc, gsVsOutRingBufDescElem0, (uint64_t)0);

            // Patch GS-VS ring buffer descriptor stride for GS output
            Value* gsVsRingBufDescElem1 = builder.CreateExtractElement(desc, (uint64_t)1);

            // Clear stride in SRD DWORD1
            SqBufRsrcWord1 strideClearMask = {};
            strideClearMask.u32All         = UINT32_MAX;
            strideClearMask.bits.stride    = 0;
            gsVsRingBufDescElem1 = builder.CreateAnd(gsVsRingBufDescElem1, builder.getInt32(strideClearMask.u32All));

            // Calculate and set stride in SRD dword1
            unsigned gsVsStride = m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices *
                                  resUsage->inOutUsage.gs.outLocCount[streamId] *
                                  sizeof(unsigned) * 4;

            SqBufRsrcWord1 strideSetValue = {};
            strideSetValue.bits.stride = gsVsStride;
            gsVsRingBufDescElem1 = builder.CreateOr(gsVsRingBufDescElem1, builder.getInt32(strideSetValue.u32All));

            desc = builder.CreateInsertElement(desc, gsVsRingBufDescElem1, (uint64_t)1);

            if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 8)
            {
                // NOTE: For GFX8+, we have to explicitly set DATA_FORMAT for GS-VS ring buffer descriptor.
                desc = setRingBufferDataFormat(desc, BUF_DATA_FORMAT_32, builder);
            }

            m_gsVsRingBufDescs[streamId] = desc;
        }
        else
        {
            // Copy shader, using GS-VS ring for input.
            assert(streamId == 0);
            m_gsVsRingBufDescs[streamId] = loadDescFromDriverTable(SiDrvTableVsRingInOffs, builder);
        }
    }
    return m_gsVsRingBufDescs[streamId];
}

// =====================================================================================================================
// Get pointers to emit counters (GS)
ArrayRef<Value*> ShaderSystemValues::getEmitCounterPtr()
{
    assert(m_shaderStage == ShaderStageGeometry);
    if (m_emitCounterPtrs.empty())
    {
        // TODO: We should only insert those offsets required by the specified input primitive.

        // Setup GS emit vertex counter
        auto& dataLayout = m_entryPoint->getParent()->getDataLayout();
        auto insertPos = &*m_entryPoint->front().getFirstInsertionPt();
        for (int i = 0; i < MaxGsStreams; ++i)
        {
            auto emitCounterPtr = new AllocaInst(Type::getInt32Ty(*m_context),
                                                  dataLayout.getAllocaAddrSpace(),
                                                  "",
                                                  insertPos);
            new StoreInst(ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                          emitCounterPtr,
                          insertPos);
            m_emitCounterPtrs.push_back(emitCounterPtr);
        }
    }
    return m_emitCounterPtrs;
}

// =====================================================================================================================
// Get descriptor table pointer
Value* ShaderSystemValues::getDescTablePtr(
    unsigned        descSet)        // Descriptor set ID
{
    if (m_descTablePtrs.size() <= descSet)
    {
        m_descTablePtrs.resize(descSet + 1);
    }
    if (m_descTablePtrs[descSet] == nullptr)
    {
        // Find the node.
        unsigned resNodeIdx = findResourceNodeByDescSet(descSet);
        if (resNodeIdx != InvalidValue)
        {
            // Get the 64-bit extended node value.
            auto descTablePtrTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_context), UINT32_MAX),
                                                    ADDR_SPACE_CONST);
            m_descTablePtrs[descSet] = getExtendedResourceNodeValue(resNodeIdx,
                                                                    descTablePtrTy,
                                                                    InvalidValue);
        }
    }
    return m_descTablePtrs[descSet];
}

// =====================================================================================================================
// Get shadow descriptor table pointer
Value* ShaderSystemValues::getShadowDescTablePtr(
    unsigned        descSet)        // Descriptor set ID
{
    if (m_shadowDescTablePtrs.size() <= descSet)
    {
        m_shadowDescTablePtrs.resize(descSet + 1);
    }
    if (m_shadowDescTablePtrs[descSet] == nullptr)
    {
        // Find the node.
        unsigned resNodeIdx = findResourceNodeByDescSet(descSet);
        if (resNodeIdx != InvalidValue)
        {
            // Get the 64-bit extended node value.
            auto descTablePtrTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_context), UINT32_MAX),
                                                    ADDR_SPACE_CONST);
            m_shadowDescTablePtrs[descSet] = getExtendedResourceNodeValue(resNodeIdx,
                                                                          descTablePtrTy,
                                                                          m_shadowDescTablePtrHigh);
        }
    }
    return m_shadowDescTablePtrs[descSet];
}

// =====================================================================================================================
// Get internal global table pointer as pointer to i8.
Value* ShaderSystemValues::getInternalGlobalTablePtr()
{
    if (m_internalGlobalTablePtr == nullptr)
    {
        auto ptrTy = Type::getInt8Ty(*m_context)->getPointerTo(ADDR_SPACE_CONST);
        // Global table is always the first function argument
        m_internalGlobalTablePtr = makePointer(getFunctionArgument(m_entryPoint, 0, "globalTable"),
                                                ptrTy,
                                                InvalidValue);
    }
    return m_internalGlobalTablePtr;
}

// =====================================================================================================================
// Get internal per shader table pointer as pointer to i8.
Value* ShaderSystemValues::getInternalPerShaderTablePtr()
{
    if (m_internalPerShaderTablePtr == nullptr)
    {
        auto ptrTy = Type::getInt8Ty(*m_context)->getPointerTo(ADDR_SPACE_CONST);
        // Per shader table is always the second function argument
        m_internalPerShaderTablePtr = makePointer(getFunctionArgument(m_entryPoint, 1, "perShaderTable"),
                                                   ptrTy,
                                                   InvalidValue);
    }
    return m_internalPerShaderTablePtr;
}

// =====================================================================================================================
// Get number of workgroups value
Value* ShaderSystemValues::getNumWorkgroups()
{
    if (m_numWorkgroups == nullptr)
    {
        Instruction* insertPos = &*m_entryPoint->front().getFirstInsertionPt();
        auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);

        auto numWorkgroupPtr = getFunctionArgument(m_entryPoint,
                                                    intfData->entryArgIdxs.cs.numWorkgroupsPtr,
                                                    "numWorkgroupsPtr");
        auto numWorkgroups = new LoadInst(numWorkgroupPtr, "", insertPos);
        numWorkgroups->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(insertPos->getContext(), {}));
        m_numWorkgroups = numWorkgroups;
    }
    return m_numWorkgroups;
}

// =====================================================================================================================
// Get spilled push constant pointer
Value* ShaderSystemValues::getSpilledPushConstTablePtr()
{
    if (m_spilledPushConstTablePtr == nullptr)
    {
        auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
        assert(intfData->pushConst.resNodeIdx != InvalidValue);
        assert(intfData->entryArgIdxs.spillTable != InvalidValue);

        Instruction* insertPos = &*m_entryPoint->front().getFirstInsertionPt();

        auto pushConstNode = &m_pipelineState->getUserDataNodes()[intfData->pushConst.resNodeIdx];
        unsigned pushConstOffset = pushConstNode->offsetInDwords * sizeof(unsigned);

        auto spillTablePtrLow = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.spillTable, "spillTable");
        auto spilledPushConstTablePtrLow = BinaryOperator::CreateAdd(spillTablePtrLow,
                                                                      ConstantInt::get(Type::getInt32Ty(*m_context),
                                                                                       pushConstOffset),
                                                                      "",
                                                                      insertPos);
        auto ty = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_context), InterfaceData::MaxSpillTableSize),
                                                 ADDR_SPACE_CONST);
        m_spilledPushConstTablePtr = makePointer(spilledPushConstTablePtrLow, ty, InvalidValue);
    }
    return m_spilledPushConstTablePtr;
}

// =====================================================================================================================
// Get vertex buffer table pointer
Value* ShaderSystemValues::getVertexBufTablePtr()
{
    if (m_vbTablePtr == nullptr)
    {
        // Find the node.
        auto vbTableNode = findResourceNodeByType(ResourceNodeType::IndirectUserDataVaPtr);
        if (vbTableNode != nullptr)
        {
            // Get the 64-bit extended node value.
            auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
            auto vbTablePtrLow = getFunctionArgument(m_entryPoint,
                                                      intfData->entryArgIdxs.vs.vbTablePtr,
                                                      "vbTablePtr");
            static const unsigned MaxVertexBufferSize = 0x10000000;
            auto vbTablePtrTy = PointerType::get(ArrayType::get(VectorType::get(Type::getInt32Ty(*m_context), 4),
                                                                 MaxVertexBufferSize),
                                                  ADDR_SPACE_CONST);
            m_vbTablePtr = makePointer(vbTablePtrLow, vbTablePtrTy, InvalidValue);
        }
    }
    return m_vbTablePtr;
}

// =====================================================================================================================
// Get stream-out buffer descriptor
Value* ShaderSystemValues::getStreamOutBufDesc(
    unsigned        xfbBuffer)      // Transform feedback buffer ID
{
    if (m_streamOutBufDescs.size() <= xfbBuffer)
    {
        m_streamOutBufDescs.resize(xfbBuffer + 1);
    }

    if (m_streamOutBufDescs[xfbBuffer] == nullptr)
    {
        auto streamOutTablePtr = getStreamOutTablePtr();
        auto insertPos = streamOutTablePtr->getNextNode();

        Value* idxs[] =
        {
            ConstantInt::get(Type::getInt64Ty(*m_context), 0),
            ConstantInt::get(Type::getInt64Ty(*m_context), xfbBuffer)
        };

        auto streamOutBufDescPtr = GetElementPtrInst::Create(nullptr, streamOutTablePtr, idxs, "", insertPos);
        streamOutBufDescPtr->setMetadata(MetaNameUniform, MDNode::get(streamOutBufDescPtr->getContext(), {}));

        auto streamOutBufDesc = new LoadInst(streamOutBufDescPtr, "", insertPos);
        streamOutBufDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(streamOutBufDesc->getContext(), {}));
        streamOutBufDesc->setAlignment(MaybeAlign(16));

        m_streamOutBufDescs[xfbBuffer] = streamOutBufDesc;
    }
    return m_streamOutBufDescs[xfbBuffer];
}

// =====================================================================================================================
// Get stream-out buffer table pointer
Instruction* ShaderSystemValues::getStreamOutTablePtr()
{
    assert((m_shaderStage == ShaderStageVertex) ||
                (m_shaderStage == ShaderStageTessEval) ||
                (m_shaderStage == ShaderStageCopyShader));

    if (m_streamOutTablePtr == nullptr)
    {
        auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
        unsigned entryArgIdx = 0;

        if (m_shaderStage != ShaderStageCopyShader)
        {
            // Find the node.
            auto node = findResourceNodeByType(ResourceNodeType::StreamOutTableVaPtr);
            if (node != nullptr)
            {
                // Get the SGPR number of the stream-out table pointer.
                switch (m_shaderStage)
                {
                case ShaderStageVertex:
                    entryArgIdx = intfData->entryArgIdxs.vs.streamOutData.tablePtr;
                    break;
                case ShaderStageTessEval:
                    entryArgIdx = intfData->entryArgIdxs.tes.streamOutData.tablePtr;
                    break;
                case ShaderStageCopyShader:
                    entryArgIdx = intfData->userDataUsage.gs.copyShaderStreamOutTable;
                    break;
                default:
                    llvm_unreachable("Should never be called!");
                    break;
                }
            }
        }
        else
        {
            // Special case code for the copy shader.
            entryArgIdx = intfData->userDataUsage.gs.copyShaderStreamOutTable;
        }

        // Get the 64-bit extended node value.
        auto streamOutTablePtrLow = getFunctionArgument(m_entryPoint, entryArgIdx, "streamOutTable");
        auto streamOutTablePtrTy = PointerType::get(ArrayType::get(VectorType::get(Type::getInt32Ty(*m_context), 4),
            MaxTransformFeedbackBuffers), ADDR_SPACE_CONST);
        m_streamOutTablePtr = makePointer(streamOutTablePtrLow, streamOutTablePtrTy, InvalidValue);
    }
    return m_streamOutTablePtr;
}

// =====================================================================================================================
// Make 64-bit pointer of specified type from 32-bit int, extending with the specified value, or PC if InvalidValue
Instruction* ShaderSystemValues::makePointer(
    Value*    lowValue,  // [in] 32-bit int value to extend
    Type*     ptrTy,     // [in] Type that result pointer needs to be
    unsigned  highValue)  // Value to use for high part, or InvalidValue to use PC
{
    // Insert extending code after pLowValue if it is an instruction.
    Instruction* insertPos = nullptr;
    auto lowValueInst = dyn_cast<Instruction>(lowValue);
    if (lowValueInst != nullptr)
    {
        insertPos = lowValueInst->getNextNode();
    }
    else
    {
        insertPos = &*m_entryPoint->front().getFirstInsertionPt();
    }

    Value* extendedPtrValue = nullptr;
    if (highValue == InvalidValue)
    {
        // Use PC.
        if ((m_pc == nullptr) || isa<Instruction>(lowValue))
        {
            // Either
            // 1. there is no existing code to s_getpc and cast it, or
            // 2. there is existing code, but pLowValue is an instruction, so it is more complex to figure
            //    out whether it is before or after pLowValue in the code. We generate new s_getpc code anyway
            //    and rely on subsequent CSE to common it up.
            // Insert the s_getpc code at the start of the function, so a later call into here knows it can
            // reuse this PC if its pLowValue is an arg rather than an instruction.
            auto pcInsertPos = &*m_entryPoint->front().getFirstInsertionPt();
            Value* pc = emitCall("llvm.amdgcn.s.getpc",
                                  Type::getInt64Ty(*m_context),
                                  ArrayRef<Value*>(),
                                  {},
                                  pcInsertPos);
            m_pc = new BitCastInst(pc, VectorType::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
        }
        else
        {
            insertPos = m_pc->getNextNode();
        }
        extendedPtrValue = m_pc;
    }
    else
    {
        // Use constant highValue value.
        Constant* elements[] =
        {
            UndefValue::get(lowValue->getType()),
            ConstantInt::get(lowValue->getType(), highValue)
        };
        extendedPtrValue = ConstantVector::get(elements);
    }
    extendedPtrValue = InsertElementInst::Create(extendedPtrValue,
                                          lowValue,
                                          ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                          "",
                                          insertPos);
    extendedPtrValue = CastInst::Create(Instruction::BitCast,
                                         extendedPtrValue,
                                         Type::getInt64Ty(*m_context),
                                         "",
                                         insertPos);
    return CastInst::Create(Instruction::IntToPtr, extendedPtrValue, ptrTy, "", insertPos);
}

// =====================================================================================================================
// Get 64-bit extended resource node value
Value* ShaderSystemValues::getExtendedResourceNodeValue(
    unsigned        resNodeIdx,     // Resource node index
    Type*           resNodeTy,     // [in] Pointer type of result
    unsigned        highValue)      // Value to use for high part, or InvalidValue to use PC
{
    return makePointer(getResourceNodeValue(resNodeIdx), resNodeTy, highValue);
}

// =====================================================================================================================
// Get 32 bit resource node value
Value* ShaderSystemValues::getResourceNodeValue(
    unsigned        resNodeIdx)     // Resource node index
{
    auto insertPos = &*m_entryPoint->front().getFirstInsertionPt();
    auto intfData   = m_pipelineState->getShaderInterfaceData(m_shaderStage);
    auto node = &m_pipelineState->getUserDataNodes()[resNodeIdx];
    Value* resNodeValue = nullptr;

    if ((node->type == ResourceNodeType::IndirectUserDataVaPtr) ||
        (node->type == ResourceNodeType::StreamOutTableVaPtr))
    {
        // Do nothing
    }
    else if ((resNodeIdx < InterfaceData::MaxDescTableCount) && (intfData->entryArgIdxs.resNodeValues[resNodeIdx] > 0))
    {
        // Resource node isn't spilled, load its value from function argument
        resNodeValue = getFunctionArgument(m_entryPoint,
                                            intfData->entryArgIdxs.resNodeValues[resNodeIdx],
                                            Twine("resNode") + Twine(resNodeIdx));
    }
    else if (node->type != ResourceNodeType::PushConst)
    {
        // Resource node is spilled, load its value from spill table
        unsigned byteOffset = node->offsetInDwords * sizeof(unsigned);

        Value* idxs[] =
        {
          ConstantInt::get(Type::getInt32Ty(*m_context), 0),
          ConstantInt::get(Type::getInt32Ty(*m_context), byteOffset)
        };
        auto spillTablePtr = getSpillTablePtr();
        insertPos = spillTablePtr->getNextNode();
        auto elemPtr = GetElementPtrInst::CreateInBounds(spillTablePtr, idxs, "", insertPos);

        Type* resNodePtrTy = nullptr;

        if  ((node->type == ResourceNodeType::DescriptorResource) ||
             (node->type == ResourceNodeType::DescriptorSampler) ||
             (node->type == ResourceNodeType::DescriptorTexelBuffer) ||
             (node->type == ResourceNodeType::DescriptorFmask) ||
             (node->type == ResourceNodeType::DescriptorBuffer) ||
             (node->type == ResourceNodeType::DescriptorBufferCompact))
        {
            resNodePtrTy = VectorType::get(Type::getInt32Ty(*m_context),
                                            node->sizeInDwords)->getPointerTo(ADDR_SPACE_CONST);
        }
        else
        {
            resNodePtrTy = Type::getInt32Ty(*m_context)->getPointerTo(ADDR_SPACE_CONST);
        }

        auto resNodePtr = BitCastInst::CreatePointerCast(elemPtr, resNodePtrTy, "", insertPos);
        resNodePtr->setMetadata(MetaNameUniform, MDNode::get(resNodePtr->getContext(), {}));

        resNodeValue = new LoadInst(resNodePtr, "", insertPos);
    }
    assert(resNodeValue != nullptr);
    return resNodeValue;
}

// =====================================================================================================================
// Get spill table pointer
Instruction* ShaderSystemValues::getSpillTablePtr()
{
    if (m_spillTablePtr == nullptr)
    {
        auto intfData   = m_pipelineState->getShaderInterfaceData(m_shaderStage);
        auto spillTablePtrLow = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.spillTable, "spillTable");
        auto spillTablePtrTy = PointerType::get(ArrayType::get(Type::getInt8Ty(*m_context),
                                                                InterfaceData::MaxSpillTableSize),
                                                 ADDR_SPACE_CONST);
        m_spillTablePtr = makePointer(spillTablePtrLow, spillTablePtrTy, InvalidValue);
    }
    return m_spillTablePtr;
}

// =====================================================================================================================
// Load descriptor from driver table
Instruction* ShaderSystemValues::loadDescFromDriverTable(
    unsigned tableOffset,    // Byte offset in driver table
    BuilderBase& builder)    // Builder to use for insertion
{
    Value* args[] =
    {
        builder.getInt32(InternalResourceTable),
        builder.getInt32(tableOffset),
        builder.getInt32(0),
    };
    return builder.createNamedCall(lgcName::DescriptorLoadBuffer,
                                   VectorType::get(Type::getInt32Ty(*m_context), 4),
                                   args,
                                   {});
}

// =====================================================================================================================
// Explicitly set the DATA_FORMAT of ring buffer descriptor.
Value* ShaderSystemValues::setRingBufferDataFormat(
    Value*          bufDesc,       // [in] Buffer Descriptor
    unsigned        dataFormat,     // Data format
    BuilderBase&    builder         // [in] Builder to use for inserting instructions
    ) const
{
    Value* elem3 = builder.CreateExtractElement(bufDesc, (uint64_t)3);

    SqBufRsrcWord3 dataFormatClearMask;
    dataFormatClearMask.u32All = UINT32_MAX;
    // TODO: This code needs to be fixed for gfx10; buffer format is handled differently.
    dataFormatClearMask.gfx6.dataFormat = 0;
    elem3 = builder.CreateAnd(elem3, builder.getInt32(dataFormatClearMask.u32All));

    SqBufRsrcWord3 dataFormatSetValue = {};
    dataFormatSetValue.gfx6.dataFormat = dataFormat;
    elem3 = builder.CreateOr(elem3, builder.getInt32(dataFormatSetValue.u32All));

    return builder.CreateInsertElement(bufDesc, elem3, (uint64_t)3);
}

// =====================================================================================================================
// Find resource node by type
const ResourceNode* ShaderSystemValues::findResourceNodeByType(
    ResourceNodeType type)           // Resource node type to find
{
    auto userDataNodes = m_pipelineState->getUserDataNodes();
    for (unsigned i = 0; i < userDataNodes.size(); ++i)
    {
        auto node = &userDataNodes[i];
        if (node->type == type)
        {
            return node;
        }
    }
    return nullptr;
}

// =====================================================================================================================
// Find resource node by descriptor set ID
unsigned ShaderSystemValues::findResourceNodeByDescSet(
    unsigned        descSet)        // Descriptor set to find
{
    auto userDataNodes = m_pipelineState->getUserDataNodes();
    for (unsigned i = 0; i < userDataNodes.size(); ++i)
    {
        auto node = &userDataNodes[i];
        if ((node->type == ResourceNodeType::DescriptorTableVaPtr) &&
              (node->innerTable[0].set == descSet))
        {
            return i;
        }
    }
    return InvalidValue;
}

// =====================================================================================================================
// Test if shadow descriptor table is enabled
bool ShaderSystemValues::isShadowDescTableEnabled() const
{
    return m_enableShadowDescTable;
}

