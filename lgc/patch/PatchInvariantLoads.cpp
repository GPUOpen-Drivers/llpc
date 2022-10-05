/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchInvariantLoads.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchInvariantLoads.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchInvariantLoads.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-invariant-loads"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char LegacyPatchInvariantLoads::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass
FunctionPass *createLegacyPatchInvariantLoads() {
  return new LegacyPatchInvariantLoads();
}

// =====================================================================================================================
LegacyPatchInvariantLoads::LegacyPatchInvariantLoads() : llvm::FunctionPass(ID) {
}

// =====================================================================================================================
// Get the analysis usage of this pass.
//
// @param [out] analysisUsage : The analysis usage.
void LegacyPatchInvariantLoads::getAnalysisUsage(AnalysisUsage &analysisUsage) const {
  analysisUsage.addRequired<LegacyPipelineStateWrapper>();
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : Function that we will patch.
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyPatchInvariantLoads::runOnFunction(Function &function) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(function.getParent());
  return m_impl.runImpl(function, pipelineState);
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : Function that we will patch.
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchInvariantLoads::run(Function &function, FunctionAnalysisManager &analysisManager) {
  const auto &moduleAnalysisManager = analysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(function);
  PipelineState *pipelineState =
      moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*function.getParent())->getPipelineState();
  if (runImpl(function, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : Function that we will patch.
// @param [in/out] pipelineState : Pipeline state object to use for this pass
// @returns : True if the function was modified by the transformation and false otherwise
bool PatchInvariantLoads::runImpl(Function &function, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Invariant-Loads\n");

  auto shaderStage = lgc::getShaderStage(&function);
  if (shaderStage == ShaderStageInvalid)
    return false;

  auto &options = pipelineState->getShaderOptions(shaderStage);
  bool clearInvariants = options.disableInvariantLoads;
  bool aggressiveInvariants = options.aggressiveInvariantLoads && !clearInvariants;

  if (!(clearInvariants || aggressiveInvariants))
    return false;

  LLVM_DEBUG(dbgs() << (clearInvariants ? "Removing invariant load flags"
                                        : "Attempting aggressive invariant load optimization")
                    << "\n";);

  std::vector<Instruction *> loads;

  for (BasicBlock &block : function) {
    for (Instruction &inst : block) {
      if (!clearInvariants && inst.mayWriteToMemory()) {
        if (IntrinsicInst *ii = dyn_cast<IntrinsicInst>(&inst)) {
          switch (ii->getIntrinsicID()) {
          case Intrinsic::amdgcn_exp:
          case Intrinsic::amdgcn_exp_compr:
          case Intrinsic::amdgcn_init_exec:
          case Intrinsic::amdgcn_init_exec_from_input:
            continue;
          default:
            break;
          }
        }
        LLVM_DEBUG(dbgs() << "Write to memory found, aborting aggressive invariant load optimization\n");
        return false;
      } else if (inst.mayReadFromMemory()) {
        loads.push_back(&inst);
      }
    }
  }

  if (loads.empty()) {
    LLVM_DEBUG(dbgs() << "Shader has no memory loads\n");
    return false;
  }

  auto &context = function.getContext();
  for (Instruction *inst : loads) {
    bool isInvariant = inst->hasMetadata(LLVMContext::MD_invariant_load);
    if (isInvariant && clearInvariants) {
      LLVM_DEBUG(dbgs() << "Removing invariant metadata: " << *inst << "\n");
      inst->setMetadata(LLVMContext::MD_invariant_load, nullptr);
    } else if (!isInvariant && !clearInvariants) {
      LLVM_DEBUG(dbgs() << "Marking load invariant: " << *inst << "\n");
      inst->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(context, None));
    }
  }

  return true;
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patch image derivative operations dependent on discards.
INITIALIZE_PASS(LegacyPatchInvariantLoads, DEBUG_TYPE, "Patch invariant loads", false, false)
