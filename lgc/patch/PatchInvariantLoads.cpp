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
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-invariant-loads"

using namespace llvm;
using namespace lgc;

namespace lgc {

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

static const unsigned UNKNOWN_ADDRESS_SPACE = ADDR_SPACE_MAX + 1;

enum AddrSpaceBit {
  ADDR_SPACE_FLAT_BIT = 1 << ADDR_SPACE_FLAT,
  ADDR_SPACE_GLOBAL_BIT = 1 << ADDR_SPACE_GLOBAL,
  ADDR_SPACE_REGION_BIT = 1 << ADDR_SPACE_REGION,
  ADDR_SPACE_LOCAL_BIT = 1 << ADDR_SPACE_LOCAL,
  ADDR_SPACE_CONST_BIT = 1 << ADDR_SPACE_CONST,
  ADDR_SPACE_PRIVATE_BIT = 1 << ADDR_SPACE_PRIVATE,
  ADDR_SPACE_CONST_32BIT_BIT = 1 << ADDR_SPACE_CONST_32BIT,
  ADDR_SPACE_BUFFER_FAT_POINTER_BIT = 1 << ADDR_SPACE_BUFFER_FAT_POINTER,
  ADDR_SPACE_UNKNOWN_BIT = 1 << UNKNOWN_ADDRESS_SPACE
};

static unsigned findAddressSpaceAccess(const Instruction *inst) {
  if (const LoadInst *li = dyn_cast<LoadInst>(inst)) {
    return std::min(li->getPointerAddressSpace(), UNKNOWN_ADDRESS_SPACE);
  } else if (const StoreInst *si = dyn_cast<StoreInst>(inst)) {
    return std::min(si->getPointerAddressSpace(), UNKNOWN_ADDRESS_SPACE);
  } else {
    if (const CallInst *ci = dyn_cast<CallInst>(inst)) {
      auto func = ci->getCalledFunction();
      if (func) {
        // Treat these as buffer address space as they do not overlap with private.
        if (func->getName().startswith("llvm.amdgcn.image") || func->getName().startswith("llvm.amdgcn.raw") ||
            func->getName().startswith("llvm.amdgcn.struct"))
          return ADDR_SPACE_BUFFER_FAT_POINTER;
      }
    }
  }
  return UNKNOWN_ADDRESS_SPACE;
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
  bool clearInvariants = options.aggressiveInvariantLoads == ClearInvariants;
  bool aggressiveInvariants = options.aggressiveInvariantLoads == EnableOptimization;

  if (options.aggressiveInvariantLoads == Auto && pipelineState->getTargetInfo().getGfxIpVersion().major >= 10) {
    switch (function.getCallingConv()) {
    case CallingConv::AMDGPU_HS:
    case CallingConv::AMDGPU_LS:
    case CallingConv::AMDGPU_GS:
    case CallingConv::AMDGPU_VS:
      LLVM_DEBUG(dbgs() << "Heuristically enable aggressive invariant load optimization\n");
      aggressiveInvariants = true;
      break;
    default:
      break;
    }
  }

  if (!(clearInvariants || aggressiveInvariants))
    return false;

  LLVM_DEBUG(dbgs() << (clearInvariants ? "Removing invariant load flags"
                                        : "Attempting aggressive invariant load optimization")
                    << "\n";);

  // This mirrors AMDGPUAliasAnalysis
  static const unsigned aliasMatrix[] = {
      /* Flat     */
      ADDR_SPACE_FLAT_BIT | ADDR_SPACE_GLOBAL_BIT | ADDR_SPACE_LOCAL_BIT | ADDR_SPACE_CONST_BIT |
          ADDR_SPACE_PRIVATE_BIT | ADDR_SPACE_CONST_32BIT_BIT | ADDR_SPACE_BUFFER_FAT_POINTER_BIT |
          ADDR_SPACE_UNKNOWN_BIT,
      /* Global   */
      ADDR_SPACE_FLAT_BIT | ADDR_SPACE_GLOBAL_BIT | ADDR_SPACE_CONST_BIT | ADDR_SPACE_CONST_32BIT_BIT |
          ADDR_SPACE_BUFFER_FAT_POINTER_BIT | ADDR_SPACE_UNKNOWN_BIT,
      /* Region   */
      ADDR_SPACE_REGION_BIT | ADDR_SPACE_UNKNOWN_BIT,
      /* Local    */
      ADDR_SPACE_FLAT_BIT | ADDR_SPACE_LOCAL_BIT | ADDR_SPACE_UNKNOWN_BIT,
      /* Constant */
      ADDR_SPACE_FLAT_BIT | ADDR_SPACE_GLOBAL_BIT | ADDR_SPACE_CONST_32BIT_BIT | ADDR_SPACE_BUFFER_FAT_POINTER_BIT |
          ADDR_SPACE_UNKNOWN_BIT,
      /* Private  */
      ADDR_SPACE_FLAT_BIT | ADDR_SPACE_PRIVATE_BIT | ADDR_SPACE_UNKNOWN_BIT,
      /* Const32  */
      ADDR_SPACE_FLAT_BIT | ADDR_SPACE_GLOBAL_BIT | ADDR_SPACE_CONST_BIT | ADDR_SPACE_BUFFER_FAT_POINTER_BIT |
          ADDR_SPACE_UNKNOWN_BIT,
      /* Buffer   */
      ADDR_SPACE_FLAT_BIT | ADDR_SPACE_GLOBAL_BIT | ADDR_SPACE_CONST_BIT | ADDR_SPACE_CONST_32BIT_BIT |
          ADDR_SPACE_BUFFER_FAT_POINTER_BIT | ADDR_SPACE_UNKNOWN_BIT,
      /* Unknown */
      ADDR_SPACE_FLAT_BIT | ADDR_SPACE_GLOBAL_BIT | ADDR_SPACE_LOCAL_BIT | ADDR_SPACE_REGION_BIT |
          ADDR_SPACE_CONST_BIT | ADDR_SPACE_PRIVATE_BIT | ADDR_SPACE_CONST_32BIT_BIT |
          ADDR_SPACE_BUFFER_FAT_POINTER_BIT | ADDR_SPACE_UNKNOWN_BIT};

  unsigned writtenAddrSpaces = 0;
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
          case Intrinsic::invariant_start:
          case Intrinsic::invariant_end:
            continue;
          default:
            break;
          }
        } else if (CallInst *ci = dyn_cast<CallInst>(&inst)) {
          auto func = ci->getCalledFunction();
          if (func && func->getName().startswith("lgc.ngg."))
            continue;
        }
        unsigned addrSpace = findAddressSpaceAccess(&inst);
        if (addrSpace == UNKNOWN_ADDRESS_SPACE) {
          LLVM_DEBUG(dbgs() << "Write to unknown memory found, aborting aggressive invariant load optimization\n");
          return false;
        }
        writtenAddrSpaces |= aliasMatrix[addrSpace];
      } else if (inst.mayReadFromMemory()) {
        loads.push_back(&inst);
      }
    }
  }

  if (loads.empty()) {
    LLVM_DEBUG(dbgs() << "Shader has no memory loads\n");
    return false;
  }

  if (clearInvariants) {
    bool changed = false;
    for (Instruction *inst : loads) {
      if (!inst->hasMetadata(LLVMContext::MD_invariant_load))
        continue;

      LLVM_DEBUG(dbgs() << "Removing invariant metadata: " << *inst << "\n");
      inst->setMetadata(LLVMContext::MD_invariant_load, nullptr);
      changed = true;
    }
    return changed;
  }

  auto &context = function.getContext();
  bool changed = false;
  for (Instruction *inst : loads) {
    if (inst->hasMetadata(LLVMContext::MD_invariant_load))
      continue;
    if (writtenAddrSpaces && (writtenAddrSpaces & (1 << findAddressSpaceAccess(inst))))
      continue;

    LLVM_DEBUG(dbgs() << "Marking load invariant: " << *inst << "\n");
    inst->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(context, {}));
    changed = true;
  }

  return changed;
}

} // namespace lgc
