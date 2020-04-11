/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchLoadScalarizer.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchLoadScalarizer.
 ***********************************************************************************************************************
 */
#include "PatchLoadScalarizer.h"
#include "PipelineShaders.h"
#include "PipelineState.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-patch-load-scalarizer"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Define static members (no initializer needed as LLVM only cares about the address of ID, never its value).
char PatchLoadScalarizer::ID;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for load scalarizer optimizations.
FunctionPass *createPatchLoadScalarizer() {
  return new PatchLoadScalarizer();
}

// =====================================================================================================================
PatchLoadScalarizer::PatchLoadScalarizer() : FunctionPass(ID) {
  m_scalarThreshold = 0;
}

// =====================================================================================================================
// Get the analysis usage of this pass.
//
// @param [out] analysisUsage : The analysis usage.
void PatchLoadScalarizer::getAnalysisUsage(AnalysisUsage &analysisUsage) const {
  analysisUsage.addRequired<PipelineStateWrapper>();
  analysisUsage.addRequired<PipelineShaders>();
  analysisUsage.addPreserved<PipelineShaders>();
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in,out] function : Function that will run this optimization.
bool PatchLoadScalarizer::runOnFunction(Function &function) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Load-Scalarizer-Opt\n");

  auto pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(function.getParent());
  auto pipelineShaders = &getAnalysis<PipelineShaders>();
  auto shaderStage = pipelineShaders->getShaderStage(&function);

  // If the function is not a valid shader stage, or the optimization is disabled, bail.
  m_scalarThreshold = 0;
  if (shaderStage != ShaderStageInvalid)
    m_scalarThreshold = pipelineState->getShaderOptions(shaderStage).loadScalarizerThreshold;
  if (m_scalarThreshold == 0)
    return false;

  m_builder.reset(new IRBuilder<>(function.getContext()));

  visit(function);

  const bool changed = (!m_instsToErase.empty());

  for (Instruction *const inst : m_instsToErase) {
    // Lastly delete any instructions we replaced.
    inst->eraseFromParent();
  }
  m_instsToErase.clear();

  return changed;
}

// =====================================================================================================================
// Visits "load" instruction.
//
// @param loadInst : The instruction
void PatchLoadScalarizer::visitLoadInst(LoadInst &loadInst) {
  const unsigned addrSpace = loadInst.getPointerAddressSpace();
  auto loadTy = dyn_cast<VectorType>(loadInst.getType());

  if (loadTy) {
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

    unsigned compCount = loadTy->getNumElements();

    if (compCount > m_scalarThreshold)
      return;

    Type *compTy = loadTy->getVectorElementType();
    uint64_t compSize = loadInst.getModule()->getDataLayout().getTypeStoreSize(compTy);

    Value *loadValue = UndefValue::get(loadTy);
    Type *newLoadPtrTy = PointerType::get(compTy, addrSpace);
    llvm::SmallVector<llvm::Value *, 4> loadComps;

    loadComps.resize(compCount);

    // Get all the metadata
    SmallVector<std::pair<unsigned, MDNode *>, 8> allMetaNodes;
    loadInst.getAllMetadata(allMetaNodes);

    m_builder->SetInsertPoint(&loadInst);
    Value *newLoadPtr = m_builder->CreateBitCast(loadInst.getPointerOperand(), newLoadPtrTy,
                                                 loadInst.getPointerOperand()->getName() + ".i0");

    for (unsigned i = 0; i < compCount; i++) {
      Value *loadCompPtr = m_builder->CreateConstGEP1_32(compTy, newLoadPtr, i,
                                                         loadInst.getPointerOperand()->getName() + ".i" + Twine(i));
      // Calculate the alignment of component i
      uint64_t compAlignment = MinAlign(loadInst.getAlignment(), i * compSize);

      loadComps[i] = m_builder->CreateAlignedLoad(compTy, loadCompPtr, MaybeAlign(compAlignment),
                                                  loadInst.getName() + ".ii" + Twine(i));

      for (auto metaNode : allMetaNodes)
        dyn_cast<Instruction>(loadComps[i])->setMetadata(metaNode.first, metaNode.second);
    }

    for (unsigned i = 0; i < compCount; i++) {
      loadValue = m_builder->CreateInsertElement(loadValue, loadComps[i], m_builder->getInt32(i),
                                                 loadInst.getName() + ".u" + Twine(i));
    }

    loadValue->takeName(&loadInst);
    loadInst.replaceAllUsesWith(loadValue);
    m_instsToErase.push_back(&loadInst);
  }
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations for load scarlarizer optimization.
INITIALIZE_PASS(PatchLoadScalarizer, DEBUG_TYPE, "Patch LLVM for load scarlarizer optimization", false, false)
