/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchImageOpCollect.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchImageOpCollect.
 ***********************************************************************************************************************
 */
#include "PatchImageOpCollect.h"
#include "lgc/state/PipelineState.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include "lgc/patch/Patch.h"

#define DEBUG_TYPE "lgc-patch-image-op-collect"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char PatchImageOpCollect::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching for image operation collecting
ModulePass *createPatchImageOpCollect() {
  return new PatchImageOpCollect();
}

// =====================================================================================================================
PatchImageOpCollect::PatchImageOpCollect() : llvm::ModulePass(ID) {
}

// =====================================================================================================================
// Get the analysis usage of this pass.
//
// @param [out] analysisUsage : The analysis usage.
void PatchImageOpCollect::getAnalysisUsage(AnalysisUsage &analysisUsage) const {
  analysisUsage.addRequired<PipelineStateWrapper>();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool PatchImageOpCollect::runOnModule(llvm::Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Image-Op-Collect\n");

  PipelineState *pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  for (Function &func : module) {
    if (!func.isIntrinsic())
      continue;
    if (func.getName().startswith("llvm.amdgcn.image")) {
      for (User *user : func.users()) {
        CallInst *call = cast<CallInst>(user);
        ShaderStage stage = getShaderStage(call->getFunction());
        ResourceUsage *resUsage = pipelineState->getShaderResourceUsage(stage);
        resUsage->useImageOp = true;
      }
    }
  }
  return false;
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for image operation collecting.
INITIALIZE_PASS(PatchImageOpCollect, DEBUG_TYPE, "Patch LLVM for image operation collecting", false, false)
