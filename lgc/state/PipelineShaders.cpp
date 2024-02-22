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
* @file  PipelineShaders.cpp
* @brief LLPC source file: contains implementation of class Llpc::PipelineShaders
***********************************************************************************************************************
*/
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-pipeline-shaders"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
AnalysisKey PipelineShaders::Key;

// =====================================================================================================================
PipelineShadersResult::PipelineShadersResult() {
  for (auto &entryPoint : m_entryPoints)
    entryPoint = nullptr;
}

// =====================================================================================================================
// Run the analysis on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : Result object of the PipelineShaders pass
PipelineShadersResult PipelineShaders::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Pipeline-Shaders\n");

  PipelineShadersResult result;

  for (auto &func : module) {
    if (isShaderEntryPoint(&func)) {
      auto shaderStage = lgc::getShaderStage(&func);

      if (shaderStage) {
        result.m_entryPoints[shaderStage.value()] = &func;
        result.m_entryPointMap[&func] = shaderStage.value();
      }
    }
  }
  return result;
}

// =====================================================================================================================
// Get the shader for a particular API shader stage, or nullptr if none
//
// @param shaderStage : Shader stage
Function *PipelineShadersResult::getEntryPoint(ShaderStageEnum shaderStage) const {
  assert((unsigned)shaderStage < ShaderStage::CountInternal);
  return m_entryPoints[shaderStage];
}

// =====================================================================================================================
// Get the ABI shader stage for a particular function, or ShaderStage::Invalid if not a shader entrypoint.
//
// @param func : Function to look up
std::optional<ShaderStageEnum> PipelineShadersResult::getShaderStage(const Function *func) const {
  auto entryMapIt = m_entryPointMap.find(func);
  if (entryMapIt != m_entryPointMap.end())
    return entryMapIt->second;
  return std::nullopt;
}
