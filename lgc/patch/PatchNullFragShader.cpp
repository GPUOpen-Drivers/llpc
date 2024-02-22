/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  PatchNullFragShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class lgc::PatchNullFragShader.
 ***********************************************************************************************************************
 */
#include "PatchNullFragShader.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/FragColorExport.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/Internal.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-null-frag-shader"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchNullFragShader::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();

  LLVM_DEBUG(dbgs() << "Run the pass Patch-Null-Frag-Shader\n");

  Patch::init(&module);

  // Do not add a null fragment shader if not generating a whole pipeline.
  if (!pipelineState->isWholePipeline())
    return PreservedAnalyses::all();

  // If a fragment shader is not needed, then do not generate one.
  const bool hasFs = pipelineState->hasShaderStage(ShaderStage::Fragment);
  if (hasFs || !pipelineState->isGraphics())
    return PreservedAnalyses::all();

  FragColorExport::generateNullFragmentShader(module, pipelineState, lgcName::NullFsEntryPoint);
  updatePipelineState(pipelineState);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Updates the the pipeline state with the data for the null fragment shader.
//
// @param [in/out] module : The LLVM module in which to add the shader.
void PatchNullFragShader::updatePipelineState(PipelineState *pipelineState) const {
  auto resUsage = pipelineState->getShaderResourceUsage(ShaderStage::Fragment);
  pipelineState->setShaderStageMask(pipelineState->getShaderStageMask() | ShaderStageMask(ShaderStage::Fragment));

  // Add usage info for dummy output
  resUsage->inOutUsage.fs.isNullFs = true;
  InOutLocationInfo origLocInfo;
  origLocInfo.setLocation(0);
  auto &newOutLocInfo = resUsage->inOutUsage.outputLocInfoMap[origLocInfo];
  newOutLocInfo.setData(InvalidValue);
}
