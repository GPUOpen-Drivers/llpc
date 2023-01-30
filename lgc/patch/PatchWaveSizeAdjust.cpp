/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-wave-size-adjust"

using namespace lgc;
using namespace llvm;

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

  if (pipelineState->getTargetInfo().getGfxIpVersion().major >= 11) {
    // Prefer Wave64 when 16-bit arithmetic is used by the shader.
    // Except when API subgroup size requirements require Wave32 or tuning option specifies wave32.
    // Check if there are any 16-bit arithmetic operations per-stage.
    bool stageChecked[ShaderStageCount] = {};
    for (auto &func : module) {
      auto shaderStage = lgc::getShaderStage(&func);
      if (shaderStage == ShaderStageInvalid || stageChecked[shaderStage])
        continue;
      if (pipelineState->getShaderWaveSize(shaderStage) == 32) {
        const auto &shaderOptions = pipelineState->getShaderOptions(shaderStage);
        if ((pipelineState->getShaderModes()->getAnyUseSubgroupSize() && shaderOptions.subgroupSize != 0) ||
            shaderOptions.waveSize != 0)
          continue;
        bool hasAny16BitArith = false;
        for (inst_iterator instIter = inst_begin(&func); instIter != inst_end(&func); ++instIter) {
          if (is16BitArithmeticOp(&*instIter)) {
            hasAny16BitArith = true;
            stageChecked[shaderStage] = true;
            break;
          }
        }
        if (hasAny16BitArith)
          pipelineState->setShaderWaveSize(shaderStage, 64);
      }
    }
  }

  return false;
}

// =====================================================================================================================
// Check if the given instruction is a 16-bit arithmetic operation
//
// @param inst : The instruction
bool PatchWaveSizeAdjust::is16BitArithmeticOp(Instruction *inst) {
  if (dyn_cast<BinaryOperator>(inst) || dyn_cast<UnaryOperator>(inst))
    return true;
  if (auto intrinsicInst = dyn_cast<IntrinsicInst>(inst)) {
    Intrinsic::ID intrinsicId = intrinsicInst->getIntrinsicID();
    switch (intrinsicId) {
    case Intrinsic::rint:
    case Intrinsic::trunc:
    case Intrinsic::fabs:
    case Intrinsic::floor:
    case Intrinsic::ceil:
    case Intrinsic::sin:
    case Intrinsic::cos:
    case Intrinsic::exp2:
    case Intrinsic::log2:
    case Intrinsic::sqrt:
    case Intrinsic::minnum:
    case Intrinsic::maxnum:
    case Intrinsic::umin:
    case Intrinsic::smin:
    case Intrinsic::umax:
    case Intrinsic::smax:
    case Intrinsic::fma:
    case Intrinsic::amdgcn_fract:
    case Intrinsic::amdgcn_frexp_mant:
    case Intrinsic::amdgcn_frexp_exp:
    case Intrinsic::amdgcn_fmed3:
    case Intrinsic::amdgcn_ldexp:
      return true;
    default:
      return false;
    }
  }
  return false;
}
