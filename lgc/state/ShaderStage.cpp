/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ShaderStage.cpp
 * @brief LLPC source file: utility functions for shader stage
 ***********************************************************************************************************************
 */
#include "ShaderStage.h"
#include "Internal.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace lgc;
using namespace llvm;

// Named metadata node used on a function to show what shader stage it is part of
namespace {
const static char ShaderStageMetadata[] = "lgc.shaderstage";
} // anonymous namespace

// =====================================================================================================================
// Set shader stage metadata on every defined function in a module
//
// @param [in/out] module : Module to set shader stage on
// @param stage : Shader stage to set
void lgc::setShaderStage(Module *module, ShaderStage stage) {
  unsigned mdKindId = module->getContext().getMDKindID(ShaderStageMetadata);
  auto stageMetaNode = MDNode::get(
      module->getContext(), {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(module->getContext()), stage))});
  for (Function &func : *module) {
    if (!func.isDeclaration())
      func.setMetadata(mdKindId, stageMetaNode);
  }
}

// =====================================================================================================================
// Set shader stage metadata on a function
//
// @param [in/out] func : Function to set shader stage on
// @param stage : Shader stage to set
void lgc::setShaderStage(Function *func, ShaderStage stage) {
  unsigned mdKindId = func->getContext().getMDKindID(ShaderStageMetadata);
  auto stageMetaNode = MDNode::get(
      func->getContext(), {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(func->getContext()), stage))});
  assert(!func->isDeclaration());
  func->setMetadata(mdKindId, stageMetaNode);
}

// =====================================================================================================================
// Gets the shader stage from the specified LLVM function. Returns ShaderStageInvalid if not shader entrypoint.
//
// @param func : LLVM function
ShaderStage lgc::getShaderStage(const Function *func) {
  // Check for the metadata that is added by PipelineState::link.
  MDNode *stageMetaNode = func->getMetadata(ShaderStageMetadata);
  if (stageMetaNode)
    return ShaderStage(mdconst::dyn_extract<ConstantInt>(stageMetaNode->getOperand(0))->getZExtValue());
  return ShaderStageInvalid;
}

// =====================================================================================================================
// Gets name string of the abbreviation for the specified shader stage
//
// @param shaderStage : Shader stage
const char *lgc::getShaderStageAbbreviation(ShaderStage shaderStage) {
  if (shaderStage == ShaderStageCopyShader)
    return "COPY";
  if (shaderStage > ShaderStageCompute)
    return "Bad";

  static const char *ShaderStageAbbrs[] = {"VS", "TCS", "TES", "GS", "FS", "CS"};
  return ShaderStageAbbrs[static_cast<unsigned>(shaderStage)];
}
