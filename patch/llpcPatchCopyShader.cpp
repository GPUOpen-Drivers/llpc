/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchCopyShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class Llpc::PatchCopyShader.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-copy-shader"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#include "llpcBuilderImpl.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcIntrinsDefs.h"
#include "llpcInternal.h"
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"

using namespace Llpc;
using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
// Pass to generate copy shader if required
class PatchCopyShader : public Patch
{
public:
    static char ID;
    PatchCopyShader() : Patch(ID)
    {
        initializePipelineShadersPass(*PassRegistry::getPassRegistry());
        initializePatchCopyShaderPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module& module) override;

    void getAnalysisUsage(AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineStateWrapper>();
        analysisUsage.addRequired<PipelineShaders>();
        // Pass does not preserve PipelineShaders as it adds a new shader.
    }

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchCopyShader);

    void ExportOutput(uint32_t streamId, IRBuilder<>& builder);
    void CollectGsGenericOutputInfo(Function* pGsEntryPoint);

    Value* CalcGsVsRingOffsetForInput(uint32_t location, uint32_t compIdx, uint32_t streamId, IRBuilder<>& builder);

    Value* LoadValueFromGsVsRing(Type* pLoadTy, uint32_t location, uint32_t streamId, IRBuilder<>& builder);

    Value* LoadGsVsRingBufferDescriptor(IRBuilder<>& builder);

    void ExportGenericOutput(Value* pOutputValue, uint32_t location, uint32_t streamId, IRBuilder<>& builder);
    void ExportBuiltInOutput(Value* pOutputValue, BuiltInKind builtInId, uint32_t streamId, IRBuilder<>& builder);

    // -----------------------------------------------------------------------------------------------------------------

    // Low part of global internal table pointer
    static const uint32_t EntryArgIdxInternalTablePtrLow = 0;

    PipelineState*        m_pPipelineState;             // Pipeline state
    GlobalVariable*       m_pLds = nullptr;             // Global variable representing LDS
    Value*                m_pGsVsRingBufDesc = nullptr; // Descriptor for GS-VS ring
};

char PatchCopyShader::ID = 0;

} // Llpc

// =====================================================================================================================
// Create pass to generate copy shader if required.
ModulePass* Llpc::CreatePatchCopyShader()
{
    return new PatchCopyShader();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool PatchCopyShader::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Copy-Shader\n");

    Patch::Init(&module);
    m_pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    auto pGsEntryPoint = pPipelineShaders->GetEntryPoint(ShaderStageGeometry);
    if (pGsEntryPoint == nullptr)
    {
        // No geometry shader -- copy shader not required.
        return false;
    }

    // Gather GS generic export details.
    CollectGsGenericOutputInfo(pGsEntryPoint);

    // Create type of new function:
    // define void @copy_shader(
    //    i32 inreg,  ; Internal table
    //    i32 inreg,  ; Shader table
    //    i32 inreg,  ; Stream-out table (GFX6-GFX8) / ES-GS size (GFX9+)
    //    i32 inreg,  ; ES-GS size (GFX6-GFX8) / Stream-out table (GFX9+)
    //    i32 inreg,  ; Stream info
    //    i32 inreg,  ; Stream-out write index
    //    i32 inreg,  ; Stream offset0
    //    i32 inreg,  ; Stream offset1
    //    i32 inreg,  ; Stream offset2
    //    i32 inreg,  ; Stream offset3
    //    i32
    IRBuilder<> builder(*m_pContext);

    auto pInt32Ty = Type::getInt32Ty(*m_pContext);
    Type* argTys[] = {  pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty,
                        pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty };
    bool argInReg[] = { true,   true,   true,   true,   true,   true,
                        true,   true,   true,   true,   false  };
    auto pEntryPointTy = FunctionType::get(builder.getVoidTy(), argTys, false);

    // Create function for the copy shader entrypoint, and insert it before the FS (if there is one).
    auto pEntryPoint = Function::Create(pEntryPointTy, GlobalValue::ExternalLinkage, LlpcName::CopyShaderEntryPoint);

    auto insertPos = module.getFunctionList().end();
    auto pFsEntryPoint = pPipelineShaders->GetEntryPoint(ShaderStageFragment);
    if (pFsEntryPoint != nullptr)
    {
        insertPos = pFsEntryPoint->getIterator();
    }
    module.getFunctionList().insert(insertPos, pEntryPoint);

    // Make the args "inreg" (passed in SGPR) as appropriate.
    for (uint32_t i = 0; i < sizeof(argInReg) / sizeof(argInReg[0]); ++i)
    {
        if (argInReg[i])
        {
            pEntryPoint->arg_begin()[i].addAttr(Attribute::InReg);
        }
    }

    // Create ending basic block, and terminate it with return.
    auto pEndBlock = BasicBlock::Create(*m_pContext, "", pEntryPoint, nullptr);
    builder.SetInsertPoint(pEndBlock);
    builder.CreateRetVoid();

    // Create entry basic block
    auto pEntryBlock = BasicBlock::Create(*m_pContext, "", pEntryPoint, pEndBlock);
    builder.SetInsertPoint(pEntryBlock);

    auto pIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageCopyShader);

    // For GFX6 ~ GFX8, streamOutTable SGPR index value should be less than esGsLdsSize
    if (m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major <= 8)
    {
        pIntfData->userDataUsage.gs.copyShaderStreamOutTable = 2;
        pIntfData->userDataUsage.gs.copyShaderEsGsLdsSize = 3;
    }
    // For GFX9+, streamOutTable SGPR index value should be greater than esGsLdsSize
    else
    {
        pIntfData->userDataUsage.gs.copyShaderStreamOutTable = 3;
        pIntfData->userDataUsage.gs.copyShaderEsGsLdsSize = 2;
    }

    if (m_pPipelineState->IsGsOnChip())
    {
        m_pLds = Patch::GetLdsVariable(m_pPipelineState, &module);
    }
    else
    {
        m_pGsVsRingBufDesc = LoadGsVsRingBufferDescriptor(builder);
    }

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageCopyShader);

    uint32_t outputStreamCount = 0;
    uint32_t outputStreamId = InvalidValue;
    for (int i = 0; i < MaxGsStreams; ++i)
    {
        if (pResUsage->inOutUsage.gs.outLocCount[i] > 0)
        {
            outputStreamCount++;
            if(outputStreamId == InvalidValue)
            {
                outputStreamId = i;
            }
        }
    }

    if ((outputStreamCount > 1) && pResUsage->inOutUsage.enableXfb)
    {
        // StreamId = streamInfo[25:24]
        auto pStreamInfo = GetFunctionArgument(pEntryPoint, CopyShaderUserSgprIdxStreamInfo);

        Value* pStreamId = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                   builder.getInt32Ty(),
                                                   {
                                                       pStreamInfo,
                                                       builder.getInt32(24),
                                                       builder.getInt32(2),
                                                   });

        //
        // .entry:
        //      switch i32 %streamId, label %.end [ i32 0, lable %stream0
        //                                          i32 1, label %stream1
        //                                          i32 2, label %stream2
        //                                          i32 3, label %stream3 ]
        //
        // .stream0:
        //      export(0)
        //      br %label .end
        //
        // .stream1:
        //      export(1)
        //      br %label .end
        //
        // .stream2:
        //      export(2)
        //      br %label .end
        //
        // .stream3:
        //      export(3)
        //      br %label .end
        //
        // .end:
        //      ret void
        //

        // Add switchInst to entry block
        auto pSwitch = builder.CreateSwitch(pStreamId, pEndBlock, outputStreamCount);

        for (uint32_t streamId = 0; streamId < MaxGsStreams; ++streamId)
        {
            if (pResUsage->inOutUsage.gs.outLocCount[streamId] > 0)
            {
                std::string blockName = ".stream" + std::to_string(streamId);
                BasicBlock* pStreamBlock = BasicBlock::Create(*m_pContext, blockName, pEntryPoint, pEndBlock);
                builder.SetInsertPoint(pStreamBlock);

                pSwitch->addCase(builder.getInt32(streamId), pStreamBlock);

                ExportOutput(streamId, builder);
                builder.CreateBr(pEndBlock);
            }
        }
    }
    else
    {
        outputStreamId = (outputStreamCount == 0) ? 0 : outputStreamId;
        ExportOutput(outputStreamId, builder);
        builder.CreateBr(pEndBlock);
    }

    // Add execution model metadata to the function.
    auto pExecModelMeta = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                                  ShaderStageCopyShader));
    auto pExecModelMetaNode = MDNode::get(*m_pContext, pExecModelMeta);
    pEntryPoint->addMetadata(LlpcName::ShaderStageMetadata, *pExecModelMetaNode);

    // Tell pipeline state there is a copy shader.
    m_pPipelineState->SetShaderStageMask(m_pPipelineState->GetShaderStageMask() | (1U << ShaderStageCopyShader));

    return true;
}

// =====================================================================================================================
// Collects info for GS generic outputs.
void PatchCopyShader::CollectGsGenericOutputInfo(
    Function* pGsEntryPoint)  // [in] Geometry shader entrypoint
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageCopyShader);

    for (auto& func : *pGsEntryPoint->getParent())
    {
        if (func.getName().startswith(LlpcName::OutputExportGeneric))
        {
            for (auto pUser : func.users())
            {
                auto pCallInst = dyn_cast<CallInst>(pUser);
                if ((pCallInst == nullptr) || (pCallInst->getParent()->getParent() != pGsEntryPoint))
                {
                    continue;
                }

                LLPC_ASSERT(pCallInst->getNumArgOperands() == 4);
                Value* pOutput = pCallInst->getOperand(pCallInst->getNumArgOperands() - 1); // Last argument
                auto pOutputTy = pOutput->getType();

                uint32_t value = cast<ConstantInt>(pCallInst->getOperand(0))->getZExtValue();
                const uint32_t streamId = cast<ConstantInt>(pCallInst->getOperand(2))->getZExtValue();

                GsOutLocInfo outLocInfo = {};
                outLocInfo.location  = value;
                outLocInfo.isBuiltIn = false;
                outLocInfo.streamId  = streamId;

                auto locMapIt = pResUsage->inOutUsage.outputLocMap.find(outLocInfo.u32All);
                if (locMapIt == pResUsage->inOutUsage.outputLocMap.end())
                {
                    continue;
                }

                uint32_t location = locMapIt->second;
                const uint32_t compIdx = cast<ConstantInt>(pCallInst->getOperand(1))->getZExtValue();

                uint32_t compCount = 1;
                auto pCompTy = pOutputTy;
                auto pOutputVecTy = dyn_cast<VectorType>(pOutputTy);
                if (pOutputVecTy != nullptr)
                {
                    compCount = pOutputVecTy->getNumElements();
                    pCompTy = pOutputVecTy->getElementType();
                }
                uint32_t bitWidth = pCompTy->getScalarSizeInBits();
                // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend
                // BYTE/WORD to DWORD and store DWORD to GS-VS ring. So for 8-bit/16-bit data type, the actual byte size
                // is based on number of DWORDs.
                bitWidth = (bitWidth < 32) ? 32 : bitWidth;
                uint32_t byteSize = bitWidth / 8 * compCount;

                LLPC_ASSERT(compIdx < 4);
                auto& genericOutByteSizes =
                    m_pPipelineState->GetShaderResourceUsage(ShaderStageCopyShader)->inOutUsage.gs.genericOutByteSizes;
                genericOutByteSizes[streamId][location].resize(4);
                genericOutByteSizes[streamId][location][compIdx] = byteSize;
            }
        }
    }
}

// =====================================================================================================================
// Exports outputs of geometry shader, inserting buffer-load/output-export calls.
void PatchCopyShader::ExportOutput(
    uint32_t        streamId,     // Export output of this stream
    IRBuilder<>&    builder)      // [in] IRBuilder to use for instruction constructing
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageCopyShader);
    auto& builtInUsage = pResUsage->builtInUsage.gs;
    const auto& genericOutByteSizes = pResUsage->inOutUsage.gs.genericOutByteSizes;

    // Export generic outputs
    for (auto& byteSizeMap : genericOutByteSizes[streamId])
    {
        // <location, <component, byteSize>>
        uint32_t loc = byteSizeMap.first;

        uint32_t byteSize = 0;
        for (uint32_t i = 0; i < 4; ++i)
        {
            byteSize += byteSizeMap.second[i];
        }

        LLPC_ASSERT(byteSize % 4 == 0);
        uint32_t dwordSize = byteSize / 4;
        Value* pOutputValue = LoadValueFromGsVsRing(
            VectorType::get(builder.getFloatTy(), dwordSize), loc, streamId, builder);
        ExportGenericOutput(pOutputValue, loc, streamId, builder);
    }

    // Export built-in outputs
    std::vector<std::pair<BuiltInKind, Type*>> builtInPairs;

    if (builtInUsage.position)
    {
        builtInPairs.push_back(std::make_pair(BuiltInPosition, VectorType::get(builder.getFloatTy(), 4)));
    }

    if (builtInUsage.pointSize)
    {
        builtInPairs.push_back(std::make_pair(BuiltInPointSize, builder.getFloatTy()));
    }

    if (builtInUsage.clipDistance > 0)
    {
        builtInPairs.push_back(std::make_pair(
            BuiltInClipDistance, ArrayType::get(builder.getFloatTy(), builtInUsage.clipDistance)));
    }

    if (builtInUsage.cullDistance > 0)
    {
        builtInPairs.push_back(std::make_pair(
            BuiltInCullDistance, ArrayType::get(builder.getFloatTy(), builtInUsage.cullDistance)));
    }

    if (builtInUsage.primitiveId)
    {
        builtInPairs.push_back(std::make_pair(BuiltInPrimitiveId, builder.getInt32Ty()));
    }

    const auto enableMultiView = m_pPipelineState->GetInputAssemblyState().enableMultiView;
    if (builtInUsage.layer || enableMultiView)
    {
        // NOTE: If mult-view is enabled, always export gl_ViewIndex rather than gl_Layer.
        builtInPairs.push_back(std::make_pair(
            enableMultiView ? BuiltInViewIndex : BuiltInLayer, builder.getInt32Ty()));
    }

    if (builtInUsage.viewportIndex)
    {
        builtInPairs.push_back(std::make_pair(BuiltInViewportIndex, builder.getInt32Ty()));
    }

    for (auto& builtInPair : builtInPairs)
    {
        auto builtInId = builtInPair.first;
        Type* pBuiltInTy = builtInPair.second;

        LLPC_ASSERT(pResUsage->inOutUsage.builtInOutputLocMap.find(builtInId) !=
            pResUsage->inOutUsage.builtInOutputLocMap.end());

        uint32_t loc = pResUsage->inOutUsage.builtInOutputLocMap[builtInId];
        Value* pOutputValue = LoadValueFromGsVsRing(pBuiltInTy, loc, streamId, builder);
        ExportBuiltInOutput(pOutputValue, builtInId, streamId, builder);
    }

    // Generate dummy gl_position vec4(0, 0, 0, 1) for the rasterization stream if transform feeback is enabled
    if (pResUsage->inOutUsage.enableXfb && (builtInUsage.position == false))
    {
        auto pZero = ConstantFP::get(builder.getFloatTy(), 0.0);
        auto pOne = ConstantFP::get(builder.getFloatTy(), 1.0);

        std::vector<Constant*> outputValues = { pZero, pZero, pZero, pOne };
        ExportBuiltInOutput(ConstantVector::get(outputValues), BuiltInPosition, streamId, builder);
    }
}

// =====================================================================================================================
// Calculates GS to VS ring offset from input location
Value* PatchCopyShader::CalcGsVsRingOffsetForInput(
    uint32_t        location,    // Output location
    uint32_t        compIdx,     // Output component
    uint32_t        streamId,    // Output stream ID
    IRBuilder<>&    builder)     // [in] IRBuilder to use for instruction constructing
{
    auto pEntryPoint = builder.GetInsertBlock()->getParent();
    Value* pVertexOffset = GetFunctionArgument(pEntryPoint, CopyShaderUserSgprIdxVertexOffset);

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageCopyShader);

    Value* pRingOffset = nullptr;
    if (m_pPipelineState->IsGsOnChip())
    {
        // ringOffset = esGsLdsSize + vertexOffset + location * 4 + compIdx
        pRingOffset = builder.getInt32(pResUsage->inOutUsage.gs.calcFactor.esGsLdsSize);
        pRingOffset = builder.CreateAdd(pRingOffset, pVertexOffset);
        pRingOffset = builder.CreateAdd(pRingOffset, builder.getInt32(location * 4 + compIdx));
    }
    else
    {
        uint32_t outputVertices = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode().outputVertices;

        // ringOffset = vertexOffset * 4 + (location * 4 + compIdx) * 64 * maxVertices
        pRingOffset = builder.CreateMul(pVertexOffset, builder.getInt32(4));
        pRingOffset = builder.CreateAdd(pRingOffset, builder.getInt32((location * 4 + compIdx) * 64 * outputVertices));
    }

    return pRingOffset;
}

// =====================================================================================================================
// Loads value from GS-VS ring (only accept 32-bit scalar, vector, or arry).
Value* PatchCopyShader::LoadValueFromGsVsRing(
    Type*           pLoadTy,    // [in] Type of the load value
    uint32_t        location,   // Output location
    uint32_t        streamId,   // Output stream ID
    IRBuilder<>&    builder)    // [in] IRBuilder to use for instruction constructing
{
    uint32_t elemCount = 1;
    Type* pElemTy = pLoadTy;

    if (pLoadTy->isArrayTy())
    {
        elemCount = pLoadTy->getArrayNumElements();
        pElemTy = pLoadTy->getArrayElementType();
    }
    else if (pLoadTy->isVectorTy())
    {
        elemCount = pLoadTy->getVectorNumElements();
        pElemTy = pLoadTy->getVectorElementType();
    }
    LLPC_ASSERT(pElemTy->isIntegerTy(32) || pElemTy->isFloatTy()); // Must be 32-bit type

#if LLPC_BUILD_GFX10
    if (m_pPipelineState->GetNggControl()->enableNgg)
    {
        // NOTE: For NGG, importing GS output from GS-VS ring is represented by a call and the call is replaced with
        // real instructions when when NGG primitive shader is generated.
        std::string callName(LlpcName::NggGsOutputImport);
        callName += GetTypeName(pLoadTy);
        return EmitCall(callName, pLoadTy,
                        {
                            builder.getInt32(location),
                            builder.getInt32(0),
                            builder.getInt32(streamId)
                        },
                        NoAttrib,
                        builder);
    }
#endif

    if (m_pPipelineState->IsGsOnChip())
    {
        LLPC_ASSERT(m_pLds != nullptr);

        Value* pRingOffset = CalcGsVsRingOffsetForInput(location, 0, streamId, builder);
        Value* pLoadPtr = builder.CreateGEP(m_pLds, { builder.getInt32(0), pRingOffset });
        pLoadPtr = builder.CreateBitCast(
            pLoadPtr, PointerType::get(pLoadTy, m_pLds->getType()->getPointerAddressSpace()));

        return builder.CreateAlignedLoad(pLoadPtr, m_pLds->getAlignment());
    }
    else
    {
        LLPC_ASSERT(m_pGsVsRingBufDesc != nullptr);

        CoherentFlag coherent = {};
        coherent.bits.glc = true;
        coherent.bits.slc = true;

        Value* pLoadValue = UndefValue::get(pLoadTy);

        for (uint32_t i = 0; i < elemCount; ++i)
        {
            Value* pRingOffset = CalcGsVsRingOffsetForInput(location + i / 4, i % 4, streamId, builder);
            auto pLoadElem = builder.CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_load,
                                                     pElemTy,
                                                     {
                                                         m_pGsVsRingBufDesc,
                                                         pRingOffset,
                                                         builder.getInt32(0),                 // soffset
                                                         builder.getInt32(coherent.u32All)    // glc, slc
                                                     });

            if (pLoadTy->isArrayTy())
            {
                pLoadValue = builder.CreateInsertValue(pLoadValue, pLoadElem, i);
            }
            else if (pLoadTy->isVectorTy())
            {
                pLoadValue = builder.CreateInsertElement(pLoadValue, pLoadElem, i);
            }
            else
            {
                LLPC_ASSERT(elemCount == 1);
                pLoadValue = pLoadElem;
            }
        }

        return pLoadValue;
    }
}

// =====================================================================================================================
// Load GS-VS ring buffer descriptor.
Value* PatchCopyShader::LoadGsVsRingBufferDescriptor(
    IRBuilder<>& builder)       // [in] IRBuilder to use for instruction constructing
{
    Function* pEntryPoint = builder.GetInsertBlock()->getParent();
    Value* pInternalTablePtrLow = GetFunctionArgument(pEntryPoint, EntryArgIdxInternalTablePtrLow);

    Value* pPc = builder.CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
    pPc = builder.CreateBitCast(pPc, VectorType::get(builder.getInt32Ty(), 2));

    auto pInternalTablePtrHigh = builder.CreateExtractElement(pPc, 1);

    auto pUndef = UndefValue::get(VectorType::get(builder.getInt32Ty(), 2));
    Value* pInternalTablePtr = builder.CreateInsertElement(pUndef, pInternalTablePtrLow, uint64_t(0));
    pInternalTablePtr = builder.CreateInsertElement(pInternalTablePtr, pInternalTablePtrHigh, 1);
    pInternalTablePtr = builder.CreateBitCast(pInternalTablePtr, builder.getInt64Ty());

    auto pGsVsRingBufDescPtr = builder.CreateAdd(pInternalTablePtr,
                                                 builder.getInt64(SI_DRV_TABLE_VS_RING_IN_OFFS << 4));

    auto pInt32x4PtrTy = PointerType::get(VectorType::get(builder.getInt32Ty(), 4), ADDR_SPACE_CONST);
    pGsVsRingBufDescPtr = builder.CreateIntToPtr(pGsVsRingBufDescPtr, pInt32x4PtrTy);
    cast<Instruction>(pGsVsRingBufDescPtr)->setMetadata(MetaNameUniform,
                                                        MDNode::get(pGsVsRingBufDescPtr->getContext(), {}));

    auto pGsVsRingBufDesc = builder.CreateLoad(pGsVsRingBufDescPtr);
    pGsVsRingBufDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(pGsVsRingBufDesc->getContext(), {}));

    return pGsVsRingBufDesc;
}

// =====================================================================================================================
// Exports generic outputs of geometry shader, inserting output-export calls.
void PatchCopyShader::ExportGenericOutput(
    Value*       pOutputValue,  // [in] Value exported to output
    uint32_t     location,      // Location of the output
    uint32_t     streamId,      // ID of output vertex stream
    IRBuilder<>& builder)       // [in] IRBuilder to use for instruction constructing
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageCopyShader);
    if (pResUsage->inOutUsage.enableXfb)
    {
        auto& outLocMap = pResUsage->inOutUsage.outputLocMap;
        auto& xfbOutsInfo = pResUsage->inOutUsage.gs.xfbOutsInfo;

        // Find original location in outLocMap which equals used location in copy shader
        auto locIter = find_if(outLocMap.begin(), outLocMap.end(), [location, streamId]
            (const std::pair<uint32_t, uint32_t>& outLoc)
        {
            uint32_t outLocInfo = outLoc.first;
            bool isStreamId = (reinterpret_cast<GsOutLocInfo*>(&outLocInfo))->streamId == streamId;
            return ((outLoc.second == location) && isStreamId);
        });

        LLPC_ASSERT(locIter != outLocMap.end());
        if (xfbOutsInfo.find(locIter->first) != xfbOutsInfo.end())
        {
            uint32_t xfbOutInfo = xfbOutsInfo[locIter->first];
            XfbOutInfo* pXfbOutInfo = reinterpret_cast<XfbOutInfo*>(&xfbOutInfo);

            if (pXfbOutInfo->is16bit)
            {
                // NOTE: For 16-bit transform feedback output, the value is 32-bit DWORD loaded from GS-VS ring
                // buffer. The high WORD is always zero while the low WORD contains the data value. We have to
                // do some casting operations before store it to transform feedback buffer (tightly packed).
                auto pOutputTy = pOutputValue->getType();
                LLPC_ASSERT(pOutputTy->isFPOrFPVectorTy() && (pOutputTy->getScalarSizeInBits() == 32));

                const uint32_t compCount = pOutputTy->isVectorTy() ? pOutputTy->getVectorNumElements() : 1;
                if (compCount > 1)
                {
                    pOutputValue = builder.CreateBitCast(pOutputValue,
                                                         VectorType::get(builder.getInt32Ty(), compCount));
                    pOutputValue = builder.CreateTrunc(pOutputValue,
                                                       VectorType::get(builder.getInt16Ty(), compCount));
                    pOutputValue = builder.CreateBitCast(pOutputValue,
                                                         VectorType::get(builder.getHalfTy(), compCount));
                }
                else
                {
                    pOutputValue = builder.CreateBitCast(pOutputValue, builder.getInt32Ty());
                    pOutputValue = new TruncInst(pOutputValue, builder.getInt16Ty());
                    pOutputValue = new BitCastInst(pOutputValue, builder.getHalfTy());
                }

            }

            Value* args[] =
            {
                builder.getInt32(pXfbOutInfo->xfbBuffer),
                builder.getInt32(pXfbOutInfo->xfbOffset),
                builder.getInt32(pXfbOutInfo->xfbExtraOffset),
                pOutputValue
            };

            std::string instName(LlpcName::OutputExportXfb);
            AddTypeMangling(nullptr, args, instName);
            EmitCall(instName, builder.getVoidTy(), args, NoAttrib, builder);
        }
    }

    if (pResUsage->inOutUsage.gs.rasterStream == streamId)
    {
        auto pOutputTy = pOutputValue->getType();
        LLPC_ASSERT(pOutputTy->isSingleValueType());

        std::string instName(LlpcName::OutputExportGeneric);
        instName += GetTypeName(pOutputTy);

        EmitCall(instName, builder.getVoidTy(), { builder.getInt32(location), pOutputValue }, NoAttrib, builder);
    }
}

// =====================================================================================================================
// Exports built-in outputs of geometry shader, inserting output-export calls.
void PatchCopyShader::ExportBuiltInOutput(
    Value*       pOutputValue,  // [in] Value exported to output
    BuiltInKind  builtInId,     // ID of the built-in variable
    uint32_t     streamId,      // ID of output vertex stream
    IRBuilder<>& builder)       // [in] IRBuilder to use for instruction constructing
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageCopyShader);

    if (pResUsage->inOutUsage.enableXfb)
    {
        GsOutLocInfo outLocInfo = {};
        outLocInfo.location  = builtInId;
        outLocInfo.isBuiltIn = true;
        outLocInfo.streamId  = streamId;

        auto& xfbOutsInfo = pResUsage->inOutUsage.gs.xfbOutsInfo;
        auto locIter = xfbOutsInfo.find(outLocInfo.u32All);
        if (locIter != xfbOutsInfo.end())
        {
            uint32_t xfbOutInfo = xfbOutsInfo[locIter->first];
            XfbOutInfo* pXfbOutInfo = reinterpret_cast<XfbOutInfo*>(&xfbOutInfo);

            std::string instName(LlpcName::OutputExportXfb);
            Value* args[] =
            {
                builder.getInt32(pXfbOutInfo->xfbBuffer),
                builder.getInt32(pXfbOutInfo->xfbOffset),
                builder.getInt32(0),
                pOutputValue
            };
            AddTypeMangling(nullptr, args, instName);
            EmitCall(instName, builder.getVoidTy(), args, NoAttrib, builder);
        }
    }

    if (pResUsage->inOutUsage.gs.rasterStream == streamId)
    {
        std::string callName = LlpcName::OutputExportBuiltIn;
        callName += BuilderImplInOut::GetBuiltInName(builtInId);
        Value* args[] = { builder.getInt32(builtInId), pOutputValue };
        AddTypeMangling(nullptr, args, callName);
        EmitCall(callName, builder.getVoidTy(), args, NoAttrib, builder);
    }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchCopyShader, DEBUG_TYPE, "Patch LLVM for copy shader generation", false, false)
