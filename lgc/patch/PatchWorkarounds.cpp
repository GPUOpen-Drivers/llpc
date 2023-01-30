/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchWorkarounds.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchWorkarounds.
 ***********************************************************************************************************************
 */

#include "lgc/patch/PatchWorkarounds.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-workarounds"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchWorkarounds::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  if (runImpl(module, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] module : Module that we will add workarounds in
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchWorkarounds::runImpl(Module &module, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Workarounds\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;
  m_builder = std::make_unique<IRBuilder<>>(*m_context);
  m_processed.clear();

  m_changed = false;

  // Patch image resource descriptor when app provides wrong type
  applyImageDescWorkaround();

  return m_changed;
}

// =====================================================================================================================
// Apply resource descriptor workaround where application is erroneously providing a buffer descriptor when it should
// be an image descriptor. We only have to apply the workaround for gfx10.1 (note: this is an application error that
// are handling gracefully)
//
void PatchWorkarounds::applyImageDescWorkaround(void) {
  if (!m_pipelineState->getOptions().disableImageResourceCheck &&
      m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waFixBadImageDescriptor) {

    // We have to consider waterfall.last.use as this may be used on a resource
    // descriptor that is then used by an image instruction.
    // waterfall.last.use needs to then be processed first due to the nature of
    // the intrinsic (dst needs to go into the processed list to prevent it
    // being processed twice, plus the workaround breaks the last.use if not
    // handled like this)

    // Build up a list of uses (since we modify we can't process them immediately)
    SmallVector<CallInst *, 8> useWorkListLastUse;
    SmallVector<CallInst *, 8> useWorkListImage;

    for (const Function &func : m_module->getFunctionList()) {
      bool isImage = func.getName().startswith("llvm.amdgcn.image");
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
      bool isLastUse = false;
#else
      bool isLastUse = func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_waterfall_last_use;
#endif
      if (isImage || isLastUse) {
        for (auto &use : func.uses()) {
          if (auto *callInst = dyn_cast<CallInst>(use.getUser())) {
            if (callInst->isCallee(&use)) {
              if (isLastUse) {
                useWorkListLastUse.push_back(callInst);
              } else {
                useWorkListImage.push_back(callInst);
              }
            }
          }
        }
      }
    }

    // Process the uses
    for (auto lastUseInst : useWorkListLastUse) {
      processImageDescWorkaround(*lastUseInst, true /* isLastUse */);
    }
    for (auto imgInst : useWorkListImage) {
      processImageDescWorkaround(*imgInst, false /* isLastUse */);
    }
  }
}

// =====================================================================================================================
// Process calls to image intrinsics and apply buffer descriptor should be image descriptor workaround
//
// @param callInst  : The image intrinsic call instruction
// @param isLastUse : The intrinsic being considered is a waterfall.last.use
void PatchWorkarounds::processImageDescWorkaround(CallInst &callInst, bool isLastUse) {
  Function *const calledFunc = callInst.getCalledFunction();
  if (!calledFunc)
    return;

  // A buffer descriptor may be incorrectly given when it should be an image descriptor, we need to fix it to valid
  // buffer type (0) to make hardware happily ignore it. This is to check and fix against buggy applications which
  // declares a image descriptor in shader but provide a buffer descriptor in driver. Note this only applies to gfx10.1.
  // Look for 8 dword resource descriptor argument
  for (Value *arg : callInst.args()) {
    if (auto vecTy = dyn_cast<FixedVectorType>(arg->getType())) {
      if (vecTy->getNumElements() == 8 && vecTy->getElementType()->isIntegerTy(32)) {
        if (isa<UndefValue>(arg))
          // We don't need to worry if the value is actually undef. This situation only really occurs in unit test
          // but either way, it is pointless to apply the workaround to an undef.
          break;

        if (m_processed.count(arg) > 0)
          // Already processed this one
          break;

        // If we are processing waterfall.last.use then additionally check that
        // the use of the descriptor is for a image intrinsic
        if (isLastUse) {
          bool requiresWorkaround = false;
          for (auto &use : callInst.uses()) {
            if (auto *useCallInst = dyn_cast<CallInst>(use.getUser())) {
              if (useCallInst->getCalledFunction()->getName().startswith("llvm.amdgcn.image")) {
                // We need to process this intrinsic
                requiresWorkaround = true;
                break;
              }
            }
          }
          if (!requiresWorkaround)
            return;
        }

        Instruction *rsrcInstr = cast<Instruction>(arg);

        m_builder->SetInsertPoint(rsrcInstr->getNextNode());
        m_builder->SetCurrentDebugLocation(rsrcInstr->getNextNode()->getDebugLoc());

        // Create a new rsrc load instruction - we apply the workaround the new instruction and then replace
        // all uses of the old one with the derived value. This prevents us replacing the original value with the
        // derived one in the code to insert the new element.
        Instruction *newInstr = rsrcInstr->clone();
        newInstr->insertAfter(rsrcInstr);

        Value *elem3 = m_builder->CreateExtractElement(newInstr, 3);
        Value *isInvalid = m_builder->CreateICmpSGE(elem3, m_builder->getInt32(0));
        Value *masked = m_builder->CreateAnd(elem3, m_builder->getInt32(0x0FFFFFFF));
        elem3 = m_builder->CreateSelect(isInvalid, masked, elem3);

        // Re-assemble descriptor
        Value *newArg = m_builder->CreateInsertElement(newInstr, elem3, 3);

        auto rsrcInstrName = rsrcInstr->getName();
        newInstr->setName(rsrcInstrName); // Preserve the old name for the load
        newArg->setName(rsrcInstrName);   // Derive a new name based on the old name for the load
        rsrcInstr->replaceAllUsesWith(newArg);
        rsrcInstr->eraseFromParent();

        // Record the new argument as already processed. If we encounter it in
        // another image intrinsic call we can skip
        m_processed.insert(newArg);

        // Additionally, if this is a last.use intrinsic, we can add the dest
        // register to the already processed list too (in fact, this is
        // required)
        if (isLastUse)
          m_processed.insert(&callInst);

        m_changed = true;
        break;
      }
    }
  }
}

} // namespace lgc
