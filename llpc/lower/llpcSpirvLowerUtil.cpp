/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerUtil.cpp
 * @brief LLPC source file: utilities for use by LLPC front-end
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerUtil.h"
#include "SPIRVInternal.h"
#include "llpcSpirvLower.h"
#include "llpcUtil.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace Llpc {

// =====================================================================================================================
// Gets the entry point (valid for AMD GPU) of a LLVM module.
//
// @param module : LLVM module
Function *getEntryPoint(Module *module) {
  Function *entryPoint = nullptr;

  for (auto func = module->begin(), end = module->end(); func != end; ++func) {
    if (!func->empty() && func->getLinkage() == GlobalValue::ExternalLinkage) {
      entryPoint = &*func;
      break;
    }
  }

  assert(entryPoint);
  return entryPoint;
}

// =====================================================================================================================
// Gets the shader stage from the specified single-shader LLVM module.
//
// @param module : LLVM module
ShaderStage getShaderStageFromModule(Module *module) {
  Function *func = getEntryPoint(module);

  // Check for the execution model metadata that is added by the SPIR-V reader.
  MDNode *execModelNode = func->getMetadata(gSPIRVMD::ExecutionModel);
  if (!execModelNode)
    return ShaderStageInvalid;
  auto execModel = mdconst::dyn_extract<ConstantInt>(execModelNode->getOperand(0))->getZExtValue();
  return convertToShaderStage(execModel);
}

// =====================================================================================================================
// Set the shader stage to the specified LLVM module entry function.
//
// @param module : LLVM module to set shader stage
// @param shaderStage : Shader stage
void setShaderStageToModule(Module *module, ShaderStage shaderStage) {
  LLVMContext &context = module->getContext();
  Function *func = getEntryPoint(module);
  auto execModel = convertToExecModel(shaderStage);
  Metadata *execModelMeta[] = {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), execModel))};
  auto execModelMetaNode = MDNode::get(context, execModelMeta);
  func->setMetadata(gSPIRVMD::ExecutionModel, execModelMetaNode);
}

// =====================================================================================================================
// Clear the block before patching the function
//
// @param func : The function to clear
BasicBlock *clearBlock(Function *func) {
  assert(func->size() == 1);
  BasicBlock &entryBlock = func->getEntryBlock();
  entryBlock.dropAllReferences();
  for (auto instIt = entryBlock.begin(); instIt != entryBlock.end();) {
    auto &inst = *instIt++;
    inst.eraseFromParent();
  }
  return &entryBlock;
}

// =====================================================================================================================
// Clear non entry external functions
// @param module : LLVM module to remove functions.
// @param entryName : Entry Function Name
void clearNonEntryFunctions(Module *module, StringRef entryName) {
  for (auto funcIt = module->begin(), funcEnd = module->end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    if (func->getLinkage() == GlobalValue::ExternalLinkage && !func->empty()) {
      if (!func->getName().startswith(entryName)) {
        func->dropAllReferences();
        func->eraseFromParent();
      }
    }
  }
}

} // namespace Llpc
