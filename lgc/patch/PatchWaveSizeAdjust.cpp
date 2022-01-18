/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/patch/PatchWaveSizeAdjust.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-wave-size-adjust"

using namespace lgc;
using namespace llvm;

char LegacyPatchWaveSizeAdjust::ID = 0;

// =====================================================================================================================
// Create PatchWaveSizeAdjust pass
//
// @param pipeline : Pipeline object
ModulePass *lgc::createLegacyPatchWaveSizeAdjust() {
  return new LegacyPatchWaveSizeAdjust();
}

// =====================================================================================================================
// Constructor
//
// @param pipeline : Pipeline object
LegacyPatchWaveSizeAdjust::LegacyPatchWaveSizeAdjust() : ModulePass(ID) {
}

// =====================================================================================================================
// Run the PatchWaveSizeAdjust pass on a module
//
// @param [in/out] module : LLVM module to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyPatchWaveSizeAdjust::runOnModule(Module &module) {
  auto pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  return m_impl.runImpl(module, pipelineState);
}

// =====================================================================================================================
// Run the PatchWaveSizeAdjust pass on a module
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchWaveSizeAdjust::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  runImpl(module, pipelineState);
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Run the PatchWaveSizeAdjust pass on a module
//
// @param module : Module to run this pass on
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchWaveSizeAdjust::runImpl(Module &module, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Running the pass of adjusting wave size heuristic\n");

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
INITIALIZE_PASS(LegacyPatchWaveSizeAdjust, DEBUG_TYPE, "Patch LLVM for per-shader wave size adjustment", false, false)
