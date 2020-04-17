/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcUtil.cpp
 * @brief LLPC source file: contains implementation of LLPC internal types and utility functions
 * (independent of LLVM use).
 ***********************************************************************************************************************
 */
#include "llpcUtil.h"
#include "llpc.h"
#include "llpcDebug.h"
#include "palPipelineAbi.h"
#include "spirvExt.h"
#include "vkgcElfReader.h"

#define DEBUG_TYPE "llpc-util"

namespace Llpc {

// =====================================================================================================================
// Gets the name string of shader stage.
//
// @param shaderStage : Shader stage
const char *getShaderStageName(ShaderStage shaderStage) {
  const char *name = nullptr;

  if (shaderStage == ShaderStageCopyShader)
    name = "copy";
  else if (shaderStage < ShaderStageCount) {
    static const char *ShaderStageNames[] = {
        "vertex", "tessellation control", "tessellation evaluation", "geometry", "fragment", "compute", "fetch"};

    name = ShaderStageNames[static_cast<unsigned>(shaderStage)];
  } else
    name = "bad";

  return name;
}

// =====================================================================================================================
// Converts the SPIR-V execution model to the shader stage
//
// @param execModel : SPIR-V execution model
ShaderStage convertToStageShage(unsigned execModel) {
  switch (execModel) {
  case spv::ExecutionModelVertex:
  case spv::ExecutionModelTessellationControl:
  case spv::ExecutionModelTessellationEvaluation:
  case spv::ExecutionModelGeometry:
  case spv::ExecutionModelFragment:
  case spv::ExecutionModelGLCompute: {
    return static_cast<ShaderStage>(execModel);
  }
  case spv::ExecutionModelCopyShader: {
    return ShaderStageCopyShader;
  }
  }

  llvm_unreachable("Should never be called!");
  return ShaderStageInvalid;
}

// =====================================================================================================================
// Converts the shader stage to the SPIR-V execution model
//
// @param shaderStage : Shader stage
spv::ExecutionModel convertToExecModel(ShaderStage shaderStage) {
  switch (shaderStage) {
  case ShaderStageVertex:
  case ShaderStageTessControl:
  case ShaderStageTessEval:
  case ShaderStageGeometry:
  case ShaderStageFragment:
  case ShaderStageCompute: {
    return static_cast<spv::ExecutionModel>(shaderStage);
  }
  case ShaderStageCopyShader: {
    return spv::ExecutionModelCopyShader;
  }
  default: {
    llvm_unreachable("Should never be called!");
    return static_cast<spv::ExecutionModel>(0);
  }
  }
}

// =====================================================================================================================
// Translates shader stage to corresponding stage mask.
//
// @param stage : Shader stage
unsigned shaderStageToMask(ShaderStage stage) {
  assert(stage < ShaderStageCount || stage == ShaderStageCopyShader);
  return (1 << stage);
}

} // namespace Llpc
