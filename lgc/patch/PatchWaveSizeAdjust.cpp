/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchWaveSizeAdjust.cpp
 * @brief LLPC source file: PatchWaveSizeAdjust pass
 ***********************************************************************************************************************
 */
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-wave-size-adjust"

using namespace lgc;
using namespace llvm;

namespace {

// =====================================================================================================================
// Pass to adjust wave size per shader stage heuristically.
class PatchWaveSizeAdjust final : public ModulePass {
public:
  PatchWaveSizeAdjust();

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineShaders>();
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.setPreservesAll();
  }

  bool runOnModule(Module &module) override;

  static char ID;

private:
  PatchWaveSizeAdjust(const PatchWaveSizeAdjust &) = delete;
  PatchWaveSizeAdjust &operator=(const PatchWaveSizeAdjust &) = delete;
};

} // namespace

char PatchWaveSizeAdjust::ID = 0;

// =====================================================================================================================
// Create PatchWaveSizeAdjust pass
//
// @param pipeline : Pipeline object
ModulePass *lgc::createPatchWaveSizeAdjust() {
  return new PatchWaveSizeAdjust();
}

// =====================================================================================================================
// Constructor
//
// @param pipeline : Pipeline object
PatchWaveSizeAdjust::PatchWaveSizeAdjust() : ModulePass(ID) {
}

// =====================================================================================================================
// Run the PatchWaveSizeAdjust pass on a module
//
// @param module : Module to run this pass on
bool PatchWaveSizeAdjust::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Running the pass of adjusting wave size heuristic\n");

  auto pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  for (int stageIdx = 0; stageIdx < ShaderStageCount; ++stageIdx) {
    ShaderStage shaderStage = static_cast<ShaderStage>(stageIdx);
    if (pipelineState->hasShaderStage(shaderStage)) {
      pipelineState->setShaderDefaultWaveSize(shaderStage);
      if (shaderStage == ShaderStageGeometry)
        pipelineState->setShaderDefaultWaveSize(ShaderStageCopyShader);
    }
  }
  return false;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchWaveSizeAdjust, DEBUG_TYPE, "Patch LLVM for per-shader wave size adjustment", false, false)
