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
 * @file  PatchImageDerivatives.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchImageDerivatives.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchImageDerivatives.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-image-derivatives"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchImageDerivatives::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  if (runImpl(module, pipelineState))
    return PreservedAnalyses::all(); // Note: this patching never invalidates analysis data
  return PreservedAnalyses::all();
}

static bool usesImplicitDerivatives(StringRef name) {
  if (!(name.startswith("llvm.amdgcn.image.sample") || name.startswith("llvm.amdgcn.image.gather")))
    return false;
  if (name.find(".l.") != std::string::npos || name.find(".d.") != std::string::npos)
    return false;
  return true;
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchImageDerivatives::runImpl(llvm::Module &module, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Image-Derivatives\n");

  if (!pipelineState->hasShaderStage(ShaderStageFragment))
    return false;
  ResourceUsage *resUsage = pipelineState->getShaderResourceUsage(ShaderStageFragment);
  if (!resUsage->builtInUsage.fs.discard)
    return false;

  SmallSet<BasicBlock *, 4> killBlocks;
  DenseSet<BasicBlock *> derivativeBlocks;

  // Find all blocks containing a kill or an image operation which uses implicit derivatives.
  for (Function &func : module) {
    if (!func.isIntrinsic())
      continue;

    const bool isKill = func.getIntrinsicID() == Intrinsic::amdgcn_kill;
    if (!(isKill || usesImplicitDerivatives(func.getName())))
      continue;

    for (User *user : func.users()) {
      CallInst *call = cast<CallInst>(user);
      // Only record blocks for fragment shader
      if (getShaderStage(call->getFunction()) != ShaderStageFragment)
        continue;

      if (isKill) {
        killBlocks.insert(call->getParent());
      } else {
        derivativeBlocks.insert(call->getParent());
      }
    }
  }

  // Note: in theory killBlocks should not be empty here, but it is cheap to check.
  if (killBlocks.empty() || derivativeBlocks.empty())
    return false;

  DenseSet<BasicBlock *> visitedBlocks;
  SmallVector<BasicBlock *> roots;
  SmallVector<BasicBlock *> worklist;

  // Establish roots from kill blocks.
  for (BasicBlock *killBlock : killBlocks) {
    // Normally a kill will be reached from a conditional branch.
    // Find the block containing the conditional branch and record it as a search root.
    // If the entry point is a reached then record it as a root.
    visitedBlocks.insert(killBlock);
    append_range(worklist, predecessors(killBlock));

    while (!worklist.empty()) {
      BasicBlock *potentialRoot = worklist.pop_back_val();
      if (visitedBlocks.count(potentialRoot))
        continue;
      visitedBlocks.insert(potentialRoot);
      if (!potentialRoot->getUniqueSuccessor() || !potentialRoot->getSingleSuccessor()) {
        roots.push_back(potentialRoot);
      } else {
        append_range(worklist, predecessors(potentialRoot));
      }
    }
  }
  assert(worklist.empty());

  // Breadth first search from roots looking for any block containing derivatives.
  visitedBlocks.clear();
  for (BasicBlock *root : roots)
    append_range(worklist, successors(root));
  while (!worklist.empty()) {
    BasicBlock *testBlock = worklist.pop_back_val();
    if (visitedBlocks.count(testBlock))
      continue;

    visitedBlocks.insert(testBlock);
    if (derivativeBlocks.count(testBlock)) {
      // Reached a derivative block; search can stop.
      // Mark fragment shader as requiring discard to demote conversion.
      LLVM_DEBUG(dbgs() << "Detected implicit derivatives used after kill.\n");
      Function *fsFunc = testBlock->getParent();
      fsFunc->addFnAttr("amdgpu-transform-discard-to-demote");
      return true;
    }

    append_range(worklist, successors(testBlock));
  }

  // No paths from kills to derivatives exist.
  return false;
}

} // namespace lgc
