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
 * @file  llpcPatchLoadScalarizer.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchLoadScalarizer.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-load-scalarizer"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "llpcPatchLoadScalarizer.h"
#include "llpcPipelineShaders.h"

using namespace Llpc;
using namespace llvm;

namespace llvm
{

namespace cl
{
// -enable-load-scalarizer: Enable the optimization for load scalarizer.
static opt<bool> EnableScalarLoad("enable-load-scalarizer",
                                   desc("Enable the optimization for load scalarizer."),
                                   init(false));
} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Define static members (no initializer needed as LLVM only cares about the address of ID, never its value).
char PatchLoadScalarizer::ID;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for load scalarizer optimizations.
FunctionPass* CreatePatchLoadScalarizer()
{
    return new PatchLoadScalarizer();
}

// =====================================================================================================================
PatchLoadScalarizer::PatchLoadScalarizer()
    :
    FunctionPass(ID)
{
    initializePatchLoadScalarizerPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Get the analysis usage of this pass.
void PatchLoadScalarizer::getAnalysisUsage(
    AnalysisUsage& analysisUsage    // [out] The analysis usage.
    ) const
{
    analysisUsage.addRequired<PipelineShaders>();
    analysisUsage.addPreserved<PipelineShaders>();
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
bool PatchLoadScalarizer::runOnFunction(
    Function& function)     // [in,out] Function that will run this optimization.
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Load-Scalarizer-Opt\n");

    bool enableLoadScalarizerPerShader = false;

    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    auto shaderStage = pPipelineShaders->GetShaderStage(&function);

    // If the function is not a valid shader stage, bail.
    if (shaderStage == ShaderStageInvalid)
    {
        return false;
    }

    m_pContext = static_cast<Context*>(&function.getContext());

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 33
    if (m_pContext->GetPipelineContext() != nullptr)
    {
        auto pShaderOptions = &(m_pContext->GetPipelineShaderInfo(shaderStage)->options);
        if (pShaderOptions->enableLoadScalarizer)
        {
            enableLoadScalarizerPerShader = true;
        }
    }
#endif

    if ((cl::EnableScalarLoad == false) && (enableLoadScalarizerPerShader == false))
    {
        return false;
    }

    m_pBuilder.reset(new IRBuilder<>(*m_pContext));

    visit(function);

    const bool changed = (m_instsToErase.empty() == false);

    for (Instruction* const pInst : m_instsToErase)
    {
        // Lastly delete any instructions we replaced.
        pInst->eraseFromParent();
    }
    m_instsToErase.clear();

    return changed;
}

// =====================================================================================================================
// Visits "load" instruction.
void PatchLoadScalarizer::visitLoadInst(
    LoadInst& loadInst) // [in] The instruction
{
    const uint32_t addrSpace = loadInst.getPointerAddressSpace();
    auto pLoadTy = dyn_cast<VectorType>(loadInst.getType());

    if (pLoadTy != nullptr)
    {
        // This optimization will try to scalarize the load inst. The pattern is like:
        //    %loadValue = load <4 x float>, <4 x float> addrspace(7)* %loadPtr, align 16
        // will be converted to:
        //    %newloadPtr = bitcast <4 x float> addrspace(7)* %loadPtr to float addrspace(7)*
        //    %loadCompPtr.i0 = getelementptr float, float addrspace(7)* %newloadPtr, i32 0
        //    %loadComp.i0 = load float, float addrspace(7)* %loadCompPtr.i0, align 16
        //    %loadCompPtr.i1 = getelementptr float, float addrspace(7)* %newloadPtr, i32 1
        //    %loadComp.i1 = load float, float addrspace(7)* %loadCompPtr.i1, align 4
        //    %loadCompPtr.i2 = getelementptr float, float addrspace(7)* %newloadPtr, i32 2
        //    %loadComp.i2 = load float, float addrspace(7)* %loadCompPtr.i2, align 8
        //    %loadCompPtr.i3 = getelementptr float, float addrspace(7)* %newloadPtr, i32 3
        //    %loadComp.i3 = load float, float addrspace(7)* %loadCompPtr.i3, align 4
        //    %loadValue.i0 = insertelement <4 x float> undef, float %loadComp.i0, i32 0
        //    %loadValue.i01 = insertelement <4 x float> %loadValue.i0, float %loadComp.i1, i32 1
        //    %loadValue.i012 = insertelement <4 x float> %loadValue.i01, float %loadComp.i2, i32 2
        //    %loadValue = insertelement <4 x float> %loadValue.i012, float %loadComp.i3, i32 3

        uint32_t compCount = pLoadTy->getNumElements();
        Type* pCompTy = pLoadTy->getVectorElementType();
        uint64_t compSize = loadInst.getModule()->getDataLayout().getTypeStoreSize(pCompTy);

        Value* pLoadValue = UndefValue::get(pLoadTy);
        Type*  pNewLoadPtrTy = PointerType::get(pCompTy, addrSpace);
        llvm::SmallVector<llvm::Value*, 4> loadComps;

        loadComps.resize(compCount);

        // Get all the metadata
        SmallVector<std::pair<uint32_t, MDNode*>, 8> allMetaNodes;
        loadInst.getAllMetadata(allMetaNodes);

        m_pBuilder->SetInsertPoint(&loadInst);
        Value* pNewLoadPtr = m_pBuilder->CreateBitCast(loadInst.getPointerOperand(),
                                                       pNewLoadPtrTy,
                                                       loadInst.getPointerOperand()->getName() + ".i0");

        for (uint32_t i = 0; i < compCount; i++)
        {
            Value* pLoadCompPtr = m_pBuilder->CreateConstGEP1_32(pCompTy,
                                                                 pNewLoadPtr,
                                                                 i,
                                                                 loadInst.getPointerOperand()->getName() + ".i" + Twine(i));
            // Calculate the alignment of component i
            uint64_t compAlignment = MinAlign(loadInst.getAlignment(), i * compSize);

            loadComps[i] = m_pBuilder->CreateAlignedLoad(pCompTy,
                                                         pLoadCompPtr,
                                                         compAlignment,
                                                         loadInst.getName() + ".ii" + Twine(i));

            for (auto metaNode : allMetaNodes)
            {
                dyn_cast<Instruction>(loadComps[i])->setMetadata(metaNode.first, metaNode.second);
            }
        }

        for (uint32_t i = 0; i < compCount; i++)
        {
            pLoadValue = m_pBuilder->CreateInsertElement(pLoadValue, loadComps[i], m_pBuilder->getInt32(i),
                loadInst.getName() + ".u" + Twine(i));
        }

        pLoadValue->takeName(&loadInst);
        loadInst.replaceAllUsesWith(pLoadValue);
        m_instsToErase.push_back(&loadInst);
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations for load scarlarizer optimization.
INITIALIZE_PASS(PatchLoadScalarizer, DEBUG_TYPE,
                "Patch LLVM for load scarlarizer optimization", false, false)

