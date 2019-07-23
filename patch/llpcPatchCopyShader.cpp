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
#include "llvm/IR/Instructions.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcIntrinsDefs.h"
#include "llpcInternal.h"
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"

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
        initializePipelineShadersPass(*llvm::PassRegistry::getPassRegistry());
        initializePatchCopyShaderPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module& module) override;

    void getAnalysisUsage(AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineShaders>();
        // Pass does not preserve PipelineShaders as it adds a new shader.
    }

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchCopyShader);

    void ExportOutput(uint32_t streamId, Instruction* pInsertPos);
    void CollectGsGenericOutputInfo(Function* pGsEntryPoint);

    llvm::Value* CalcGsVsRingOffsetForInput(uint32_t           location,
                                            uint32_t           compIdx,
                                            uint32_t           streamId,
                                            llvm::Instruction* pInsertPos);

    llvm::Value* LoadValueFromGsVsRingBuffer(uint32_t           location,
                                             uint32_t           compIdx,
                                             uint32_t           streamId,
                                             llvm::Instruction* pInsertPos);

    llvm::Value* LoadGsVsRingBufferDescriptor(llvm::Function* pEntryPoint, llvm::Instruction* pInsertPos);

    void ExportGenericOutput(llvm::Value*       pOutputValue,
                             uint32_t           location,
                             uint32_t           streamId,
                             llvm::Instruction* pInsertPos);

    void ExportBuiltInOutput(llvm::Value*       pOutputValue,
                             spv::BuiltIn       builtInId,
                             uint32_t           streamId,
                             llvm::Instruction* pInsertPos);

    // -----------------------------------------------------------------------------------------------------------------

    // Low part of global internal table pointer
    static const uint32_t EntryArgIdxInternalTablePtrLow = 0;

    GlobalVariable*       m_pLds;                 // Global variable representing LDS
    Value*                m_pGsVsRingBufDesc;   // Descriptor for GS-VS ring
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
    llvm::Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Copy-Shader\n");

    Patch::Init(&module);
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    auto pGsEntryPoint = pPipelineShaders->GetEntryPoint(ShaderStageGeometry);
    if (pGsEntryPoint == nullptr)
    {
        // No geometry shader -- copy shader not required.
        return false;
    }

#if LLPC_BUILD_GFX10
    if (m_pContext->GetNggControl()->enableNgg)
    {
        // No copy shader for NGG.
        return false;
    }
#endif

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
    auto pInt32Ty = m_pContext->Int32Ty();
    Type* argTys[] = {  pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty,
                        pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty, pInt32Ty };
    bool argInReg[] = { true,   true,   true,   true,   true,   true,
                        true,   true,   true,   true,   false  };
    auto pEntryPointTy = FunctionType::get(m_pContext->VoidTy(), argTys, false);

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
    ReturnInst::Create(*m_pContext, pEndBlock);

    // Create entry basic block
    auto pEntryBlock = BasicBlock::Create(*m_pContext, "", pEntryPoint, pEndBlock);
    auto pInsertPos = BranchInst::Create(pEndBlock, pEntryBlock);

    auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageCopyShader);

    const auto gfxIp = m_pContext->GetGfxIpVersion();

    // For GFX6 ~ GFX8, streamOutTable SGPR index value should be less than esGsLdsSize
    if (gfxIp.major <= 8)
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

    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageCopyShader);

    std::vector<Value*> args;

    // Load GS-VS ring buffer descriptor
    m_pGsVsRingBufDesc = LoadGsVsRingBufferDescriptor(pEntryPoint, pInsertPos);

    if (m_pContext->IsGsOnChip())
    {
        m_pLds = Patch::GetLdsVariable(&module);
    }

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

        args.clear();
        args.push_back(pStreamInfo);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 24));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 2));
        Value* pStreamId = EmitCall(m_pModule,
                                    "llvm.amdgcn.ubfe.i32",
                                    m_pContext->Int32Ty(),
                                    args,
                                    NoAttrib,
                                    &*pInsertPos);

        //
        // .entry:
        //      br label %.switch
        // .switch:
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

        // Remove entry block terminator
        auto pTerminator = pEntryBlock->getTerminator();
        pTerminator->removeFromParent();
        pTerminator->dropAllReferences();

        // Add switchInst to entry block
        auto pSwitch = SwitchInst::Create(pStreamId, pEndBlock, outputStreamCount, pEntryBlock);

        for (uint32_t streamId = 0; streamId < MaxGsStreams; ++streamId)
        {
            if (pResUsage->inOutUsage.gs.outLocCount[streamId] > 0)
            {
                std::string streamName = ".stream" + std::to_string(streamId);
                BasicBlock *pStreamBlock = BasicBlock::Create(*m_pContext, streamName, pEntryPoint, pEndBlock);
                BranchInst::Create(pEndBlock, pStreamBlock);

                pSwitch->addCase(
                    ConstantInt::get(dyn_cast<IntegerType>(m_pContext->Int32Ty()), streamId), pStreamBlock);

                ExportOutput(streamId, &*pStreamBlock->getFirstInsertionPt());
            }
        }
    }
    else
    {
        outputStreamId = (outputStreamCount == 0) ? 0 : outputStreamId;
        ExportOutput(outputStreamId, pInsertPos);
    }

    // Add SPIR-V execution model metadata to the function.
    auto pExecModelMeta = ConstantAsMetadata::get(ConstantInt::get(m_pContext->Int32Ty(), ExecutionModelCopyShader));
    auto pExecModelMetaNode = MDNode::get(*m_pContext, pExecModelMeta);
    pEntryPoint->addMetadata(gSPIRVMD::ExecutionModel, *pExecModelMetaNode);

    return true;
}

// =====================================================================================================================
// Collects info for GS generic outputs.
void PatchCopyShader::CollectGsGenericOutputInfo(
    Function* pGsEntryPoint)  // [in] Geometry shader entrypoint
{
    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageCopyShader);

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
                    m_pContext->GetShaderResourceUsage(ShaderStageCopyShader)->inOutUsage.gs.genericOutByteSizes;
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
    Instruction*    pInsertPos)   // [in] Where to insert the instruction
{
    std::vector<Value*> args;

    std::string instName;

    Value* pOutputValue = nullptr;

    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageCopyShader);
    auto& builtInUsage = pResUsage->builtInUsage.gs;
    const auto& genericOutByteSizes = pResUsage->inOutUsage.gs.genericOutByteSizes;

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
        auto pOutputTy = VectorType::get(m_pContext->FloatTy(), dwordSize);
        pOutputValue = UndefValue::get(pOutputTy);

        for (uint32_t i = 0; i < dwordSize; ++i)
        {
            auto pLoadValue = LoadValueFromGsVsRingBuffer(loc + i / 4, i % 4, streamId, pInsertPos);
            pOutputValue = InsertElementInst::Create(pOutputValue,
                                                     pLoadValue,
                                                     ConstantInt::get(m_pContext->Int32Ty(), i),
                                                     "",
                                                     pInsertPos);
        }
        ExportGenericOutput(pOutputValue, loc, streamId, pInsertPos);
    }

    pOutputValue = UndefValue::get(m_pContext->Floatx4Ty());
    if (builtInUsage.position)
    {
        LLPC_ASSERT(pResUsage->inOutUsage.builtInOutputLocMap.find(BuiltInPosition) !=
            pResUsage->inOutUsage.builtInOutputLocMap.end());

        uint32_t loc = pResUsage->inOutUsage.builtInOutputLocMap[BuiltInPosition];

        for (uint32_t i = 0; i < 4; ++i)
        {
            auto pLoadValue = LoadValueFromGsVsRingBuffer(loc, i, streamId, pInsertPos);
                                                          pOutputValue = InsertElementInst::Create(pOutputValue,
                                                          pLoadValue,
                                                          ConstantInt::get(m_pContext->Int32Ty(), i),
                                                          "",
                                                          pInsertPos);
        }
        ExportBuiltInOutput(pOutputValue, BuiltInPosition, streamId, pInsertPos);
    }
    // Generate dummy gl_position vec4(0, 0, 0, 1) for the raster stream
    else if (pResUsage->inOutUsage.enableXfb)
    {
        auto pZero = ConstantFP::get(m_pContext->FloatTy(), 0.0);
        auto pOne = ConstantFP::get(m_pContext->FloatTy(), 1.0);
        pOutputValue = InsertElementInst::Create(pOutputValue,
                                                 pZero,
                                                 ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                 "",
                                                 pInsertPos);

        pOutputValue = InsertElementInst::Create(pOutputValue,
                                                 pZero,
                                                 ConstantInt::get(m_pContext->Int32Ty(), 1),
                                                 "",
                                                 pInsertPos);

        pOutputValue = InsertElementInst::Create(pOutputValue,
                                                 pZero,
                                                 ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                 "",
                                                 pInsertPos);

        pOutputValue = InsertElementInst::Create(pOutputValue,
                                                 pOne,
                                                 ConstantInt::get(m_pContext->Int32Ty(), 3),
                                                 "",
                                                 pInsertPos);

        ExportBuiltInOutput(pOutputValue, BuiltInPosition, streamId, pInsertPos);
    }

    if (builtInUsage.pointSize)
    {
        LLPC_ASSERT(pResUsage->inOutUsage.builtInOutputLocMap.find(BuiltInPointSize) !=
            pResUsage->inOutUsage.builtInOutputLocMap.end());

        uint32_t loc = pResUsage->inOutUsage.builtInOutputLocMap[BuiltInPointSize];
        auto pLoadValue = LoadValueFromGsVsRingBuffer(loc, 0, streamId, pInsertPos);

        ExportBuiltInOutput(pLoadValue, BuiltInPointSize, streamId, pInsertPos);
    }

    if (builtInUsage.clipDistance > 0)
    {
        LLPC_ASSERT(pResUsage->inOutUsage.builtInOutputLocMap.find(BuiltInClipDistance) !=
            pResUsage->inOutUsage.builtInOutputLocMap.end());

        uint32_t loc = pResUsage->inOutUsage.builtInOutputLocMap[BuiltInClipDistance];
        pOutputValue = UndefValue::get(ArrayType::get(m_pContext->FloatTy(), builtInUsage.clipDistance));

        for (uint32_t i = 0; i < builtInUsage.clipDistance; ++i)
        {
            auto pLoadValue = LoadValueFromGsVsRingBuffer(loc + i / 4, i % 4, streamId, pInsertPos);
            std::vector<uint32_t> idxs;
            idxs.push_back(i);
            pOutputValue = InsertValueInst::Create(pOutputValue,
                                                   pLoadValue,
                                                   idxs,
                                                   "",
                                                   pInsertPos);
        }

        ExportBuiltInOutput(pOutputValue, BuiltInClipDistance, streamId, pInsertPos);
    }

    if (builtInUsage.cullDistance > 0)
    {
        LLPC_ASSERT(pResUsage->inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) !=
            pResUsage->inOutUsage.builtInOutputLocMap.end());

        uint32_t loc = pResUsage->inOutUsage.builtInOutputLocMap[BuiltInCullDistance];
        pOutputValue = UndefValue::get(ArrayType::get(m_pContext->FloatTy(), builtInUsage.cullDistance));

        for (uint32_t i = 0; i < builtInUsage.cullDistance; ++i)
        {
            auto pLoadValue = LoadValueFromGsVsRingBuffer(loc + i / 4, i % 4, streamId, pInsertPos);

            std::vector<uint32_t> idxs;
            idxs.push_back(i);
            pOutputValue = InsertValueInst::Create(pOutputValue,
                                                   pLoadValue,
                                                   idxs,
                                                   "",
                                                   pInsertPos);
        }

        ExportBuiltInOutput(pOutputValue, BuiltInCullDistance, streamId, pInsertPos);
    }

    if (builtInUsage.primitiveId)
    {
        LLPC_ASSERT(pResUsage->inOutUsage.builtInOutputLocMap.find(BuiltInPrimitiveId) !=
            pResUsage->inOutUsage.builtInOutputLocMap.end());

        uint32_t loc = pResUsage->inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId];

        auto pLoadValue = LoadValueFromGsVsRingBuffer(loc, 0, streamId, pInsertPos);
        pLoadValue = new BitCastInst(pLoadValue, m_pContext->Int32Ty(), "", pInsertPos);

        ExportBuiltInOutput(pLoadValue, BuiltInPrimitiveId, streamId, pInsertPos);
    }

    const auto enableMultiView = (reinterpret_cast<const GraphicsPipelineBuildInfo*>(
        m_pContext->GetPipelineBuildInfo()))->iaState.enableMultiView;
    if (builtInUsage.layer || enableMultiView)
    {
        // NOTE: If mult-view is enabled, always export gl_ViewIndex rather than gl_Layer.
        auto builtInId = enableMultiView ? BuiltInViewIndex : BuiltInLayer;
        auto& builtInOutLocMap = pResUsage->inOutUsage.builtInOutputLocMap;
        LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());

        uint32_t loc = builtInOutLocMap[builtInId];

        auto pLoadValue = LoadValueFromGsVsRingBuffer(loc, 0, streamId, pInsertPos);
        pLoadValue = new BitCastInst(pLoadValue, m_pContext->Int32Ty(), "", pInsertPos);

        ExportBuiltInOutput(pLoadValue, BuiltInLayer, streamId, pInsertPos);
    }

    if (builtInUsage.viewportIndex)
    {
        LLPC_ASSERT(pResUsage->inOutUsage.builtInOutputLocMap.find(BuiltInViewportIndex) !=
            pResUsage->inOutUsage.builtInOutputLocMap.end());

        uint32_t loc = pResUsage->inOutUsage.builtInOutputLocMap[BuiltInViewportIndex];
        auto pLoadValue = LoadValueFromGsVsRingBuffer(loc, 0, streamId, pInsertPos);
        pLoadValue = new BitCastInst(pLoadValue, m_pContext->Int32Ty(), "", pInsertPos);

        ExportBuiltInOutput(pLoadValue, BuiltInViewportIndex, streamId, pInsertPos);
    }

}

// =====================================================================================================================
// Calculates GS to VS ring offset from input location
Value* PatchCopyShader::CalcGsVsRingOffsetForInput(
    uint32_t        location,    // Output location
    uint32_t        compIdx,     // Output component
    uint32_t        streamId,    // Output stream ID
    Instruction*    pInsertPos)  // [in] Where to insert the instruction
{
    auto pEntryPoint = pInsertPos->getParent()->getParent();
    Value* pVertexOffset = GetFunctionArgument(pEntryPoint, CopyShaderUserSgprIdxVertexOffset);

    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageCopyShader);

    Value* pRingOffset = nullptr;
    if (m_pContext->IsGsOnChip())
    {
        // ringOffset = esGsLdsSize + vertexOffset + location * 4 + compIdx
        pRingOffset = ConstantInt::get(m_pContext->Int32Ty(), pResUsage->inOutUsage.gs.calcFactor.esGsLdsSize);

        pRingOffset = BinaryOperator::CreateAdd(pRingOffset, pVertexOffset, "", pInsertPos);

        pRingOffset = BinaryOperator::CreateAdd(pRingOffset,
                                                ConstantInt::get(m_pContext->Int32Ty(), (location * 4) + compIdx),
                                                "",
                                                pInsertPos);
    }
    else
    {
        uint32_t outputVertices = pResUsage->builtInUsage.gs.outputVertices;

        // ringOffset = vertexOffset * 4 + (location * 4 + compIdx) * 64 * maxVertices
        pRingOffset = BinaryOperator::CreateMul(pVertexOffset,
                                                ConstantInt::get(m_pContext->Int32Ty(), 4),
                                                "",
                                                pInsertPos);

        pRingOffset = BinaryOperator::CreateAdd(pRingOffset,
                                                ConstantInt::get(m_pContext->Int32Ty(),
                                                                 (location * 4 + compIdx) * 64 *
                                                                 outputVertices),
                                                "",
                                                pInsertPos);
    }

    return pRingOffset;
}

// =====================================================================================================================
// // Loads value from GS-VS ring buffer.
Value* PatchCopyShader::LoadValueFromGsVsRingBuffer(
    uint32_t        location,   // Output location
    uint32_t        compIdx,    // Output component
    uint32_t        streamId,   // Output stream ID
    Instruction*    pInsertPos) // [in] Where to insert the load instruction
{
    Value* pLoadValue = nullptr;
    Value* pRingOffset = CalcGsVsRingOffsetForInput(location, compIdx, streamId, pInsertPos);

    if (m_pContext->IsGsOnChip())
    {
        std::vector<Value*> idxs;
        idxs.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
        idxs.push_back(pRingOffset);

        Value* pLoadPtr = GetElementPtrInst::Create(nullptr, m_pLds, idxs, "", pInsertPos);
        pLoadValue = new LoadInst(pLoadPtr, "", false, m_pLds->getAlignment(), pInsertPos);

        pLoadValue = new BitCastInst(pLoadValue, m_pContext->FloatTy(), "", pInsertPos);
    }
    else
    {
        std::vector<Value*> args;
        args.push_back(m_pGsVsRingBufDesc);                                         // rsrc
        args.push_back(pRingOffset);                                                // offset
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));                 // soffset
        CoherentFlag coherent = {};
        coherent.bits.glc = true;
        coherent.bits.slc = true;
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), coherent.u32All));   // glc, slc

        pLoadValue = EmitCall(m_pModule,
                              "llvm.amdgcn.raw.buffer.load.f32",
                              m_pContext->FloatTy(),
                              args,
                              NoAttrib,
                              pInsertPos);
    }

    return pLoadValue;
}

// =====================================================================================================================
// Load GS-VS ring buffer descriptor.
Value* PatchCopyShader::LoadGsVsRingBufferDescriptor(
    Function*    pEntryPoint,   // [in] Entry-point of copy shader
    Instruction* pInsertPos)    // [in] Where to insert instructions
{
    Value* pInternalTablePtrLow = GetFunctionArgument(pEntryPoint, EntryArgIdxInternalTablePtrLow);

    std::vector<Value*> args;
    Value* pPc = EmitCall(m_pModule, "llvm.amdgcn.s.getpc", m_pContext->Int64Ty(), args, NoAttrib, pInsertPos);
    pPc = new BitCastInst(pPc, m_pContext->Int32x2Ty(), "", &*pInsertPos);

    auto pInternalTablePtrHigh =
        ExtractElementInst::Create(pPc, ConstantInt::get(m_pContext->Int32Ty(), 1), "", pInsertPos);

    Value* pInternalTablePtr = InsertElementInst::Create(UndefValue::get(m_pContext->Int32x2Ty()),
                                                         pInternalTablePtrLow,
                                                         ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                         "",
                                                         pInsertPos);
    pInternalTablePtr = InsertElementInst::Create(pInternalTablePtr,
                                                  pInternalTablePtrHigh,
                                                  ConstantInt::get(m_pContext->Int32Ty(), 1),
                                                  "",
                                                  pInsertPos);
    pInternalTablePtr = new BitCastInst(pInternalTablePtr, m_pContext->Int64Ty(), "", pInsertPos);

    Instruction* pGsVsRingBufDescPtr =
        BinaryOperator::CreateShl(ConstantInt::get(m_pContext->Int64Ty(), SI_DRV_TABLE_VS_RING_IN_OFFS),
                                  ConstantInt::get(m_pContext->Int64Ty(), 4),
                                  "",
                                  pInsertPos);
    pGsVsRingBufDescPtr = BinaryOperator::CreateAdd(pInternalTablePtr,
                                                    pGsVsRingBufDescPtr,
                                                    "",
                                                    pInsertPos);

    // NOTE: The pass of mutating address space will translate SPIR-V address space to AMDGPU space later.
    pGsVsRingBufDescPtr = new IntToPtrInst(pGsVsRingBufDescPtr,
                                           PointerType::get(m_pContext->Int32x4Ty(), SPIRAS_Constant),
                                           "",
                                           pInsertPos);
    pGsVsRingBufDescPtr->setMetadata(m_pContext->MetaIdUniform(), m_pContext->GetEmptyMetadataNode());
    pGsVsRingBufDescPtr->setMetadata(m_pContext->MetaIdDereferenceable(), m_pContext->GetEmptyMetadataNode());

    auto pGsVsRingBufDesc = new LoadInst(pGsVsRingBufDescPtr, "", pInsertPos);
    pGsVsRingBufDesc->setMetadata(m_pContext->MetaIdInvariantLoad(), m_pContext->GetEmptyMetadataNode());

    return pGsVsRingBufDesc;
}

// =====================================================================================================================
// Exports generic outputs of geometry shader, inserting output-export calls.
void PatchCopyShader::ExportGenericOutput(
    Value*       pOutputValue,  // [in] Value exported to output
    uint32_t     location,      // Location of the output
    uint32_t     streamId,      // ID of output vertex stream
    Instruction* pInsertPos)    // [in] Where to insert the instructions
{
    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageCopyShader);

    std::vector<Value*> args;
    std::string instName;

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
                    pOutputValue = new BitCastInst(pOutputValue,
                                                   VectorType::get(m_pContext->Int32Ty(), compCount),
                                                   "",
                                                   pInsertPos);
                    pOutputValue = new TruncInst(pOutputValue,
                                                 VectorType::get(m_pContext->Int16Ty(), compCount),
                                                 "",
                                                 pInsertPos);
                    pOutputValue = new BitCastInst(pOutputValue,
                                                   VectorType::get(m_pContext->Float16Ty(), compCount),
                                                   "",
                                                   pInsertPos);
                }
                else
                {
                    pOutputValue = new BitCastInst(pOutputValue, m_pContext->Int32Ty(), "", pInsertPos);
                    pOutputValue = new TruncInst(pOutputValue, m_pContext->Int16Ty(), "", pInsertPos);
                    pOutputValue = new BitCastInst(pOutputValue, m_pContext->Float16Ty(), "", pInsertPos);
                }

            }

            args.clear();
            instName = LlpcName::OutputExportXfb;
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), pXfbOutInfo->xfbBuffer));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), pXfbOutInfo->xfbOffset));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), pXfbOutInfo->xfbLocOffset));
            args.push_back(pOutputValue);
            AddTypeMangling(nullptr, args, instName);
            EmitCall(m_pModule, instName, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);
        }
    }

    if (pResUsage->inOutUsage.gs.rasterStream == streamId)
    {
        auto pOutputTy = pOutputValue->getType();
        LLPC_ASSERT(pOutputTy->isSingleValueType());

        args.clear();
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), location));
        args.push_back(pOutputValue);

        instName = LlpcName::OutputExportGeneric;
        instName += GetTypeName(pOutputTy);

        EmitCall(m_pModule, instName, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);
    }
}

// =====================================================================================================================
// Exports built-in outputs of geometry shader, inserting output-export calls.
void PatchCopyShader::ExportBuiltInOutput(
    Value*       pOutputValue,  // [in] Value exported to output
    BuiltIn      builtInId,     // ID of the built-in variable
    uint32_t     streamId,      // ID of output vertex stream
    Instruction* pInsertPos)    // [in] Where to insert the instructions
{
    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageCopyShader);

    std::vector<Value*> args;
    std::string instName;

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

            args.clear();
            instName = LlpcName::OutputExportXfb;
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), pXfbOutInfo->xfbBuffer));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), pXfbOutInfo->xfbOffset));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
            args.push_back(pOutputValue);
            AddTypeMangling(nullptr, args, instName);
            EmitCall(m_pModule, instName, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);
        }
    }

    if (pResUsage->inOutUsage.gs.rasterStream == streamId)
    {
        args.clear();
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), builtInId));
        args.push_back(pOutputValue);

        std::string builtInName = getNameMap(builtInId).map(builtInId);
        LLPC_ASSERT(builtInName.find("BuiltIn") == 0);
        instName = LlpcName::OutputExportBuiltIn + builtInName.substr(strlen("BuiltIn"));
        EmitCall(m_pModule, instName, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);
    }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchCopyShader, DEBUG_TYPE, "Patch LLVM for copy shader generation", false, false)
