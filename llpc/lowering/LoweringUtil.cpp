/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LoweringUtil.cpp
 * @brief LLPC source file: utilities for use by LLPC front-end
 ***********************************************************************************************************************
 */

#include "LoweringUtil.h"
#include "Lowering.h"
#include "SPIRVInternal.h"
#include "llpcUtil.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace Llpc {

// =====================================================================================================================
// Gets all entry points of a LLVM module.
//
// @param module : LLVM module
// @param result : Vector that all entries are appended to.
void getEntryPoints(Module *module, SmallVectorImpl<Function *> &result) {
  for (auto func = module->begin(), end = module->end(); func != end; ++func) {
    if (!func->empty() && func->getLinkage() == GlobalValue::ExternalLinkage) {
      result.push_back(&*func);
    }
  }
}

// =====================================================================================================================
// Gets the unique entry point (valid for AMD GPU) of a LLVM module.
//
// @param module : LLVM module
Function *getEntryPoint(Module *module) {
  SmallVector<Function *, 1> entries;
  getEntryPoints(module, entries);
  assert(entries.size() == 1);
  return entries[0];
}

// =====================================================================================================================
// Gets the shader stage from the specified single-shader LLVM function.
//
// @param module : LLVM module
ShaderStage getShaderStageFromFunction(Function *function) {
  // Check for the execution model metadata that is added by the SPIR-V reader.
  MDNode *execModelNode = function->getMetadata(gSPIRVMD::ExecutionModel);
  if (!execModelNode)
    return ShaderStageInvalid;

  auto execModel = mdconst::extract<ConstantInt>(execModelNode->getOperand(0))->getZExtValue();
  return convertToShaderStage(execModel);
}

// =====================================================================================================================
// Gets the shader stage from the specified single-shader LLVM module.
//
// @param module : LLVM module
ShaderStage getShaderStageFromModule(Module *module) {
  // When processing the GpuRt module, there can initially be multiple entries,
  // so we can't use getEntryPoint.
  SmallVector<Function *> entries;
  getEntryPoints(module, entries);

  std::optional<ShaderStage> result;

  for (Function *func : entries) {
    ShaderStage funcStage = getShaderStageFromFunction(func);
    assert(funcStage == result.value_or(funcStage));
    result = funcStage;
#ifdef NDEBUG
    break;
#endif
  }
  return result.value();
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
    if ((func->getLinkage() == GlobalValue::ExternalLinkage || func->getLinkage() == GlobalValue::WeakAnyLinkage) &&
        !func->empty()) {
      if (!func->getName().starts_with(entryName)) {
        func->dropAllReferences();
        func->eraseFromParent();
      }
    }
  }
}

// =====================================================================================================================
// Get in/out meta data recursively.
//
// @param [in]  valueTy : The metadata's embellish type.
// @param [in]  mds     : The metadata constant of InOut Global variable to be decode.
// @param [out] out     : Use to output the element's metadatas of the InOut Global variable.
void decodeInOutMetaRecursively(llvm::Type *valueTy, llvm::Constant *mds, llvm::SmallVector<ShaderInOutMetadata> &out) {
  ShaderInOutMetadata md = {};
  if (valueTy->isSingleValueType()) {
    // Single type's metadata:{uint64, uint64}
    md.U64All[0] = cast<ConstantInt>(mds->getOperand(0))->getZExtValue();
    md.U64All[1] = cast<ConstantInt>(mds->getOperand(1))->getZExtValue();
    out.push_back(md);
  } else if (valueTy->isArrayTy()) {
    // Array type's metadata:{uint32, {element metadata type}, uint64, uint64}
    assert(mds->getType()->getStructNumElements() == 4);
    decodeInOutMetaRecursively(valueTy->getArrayElementType(), cast<Constant>(mds->getOperand(1)), out);
    md.U64All[0] = cast<ConstantInt>(mds->getOperand(2))->getZExtValue();
    md.U64All[1] = cast<ConstantInt>(mds->getOperand(3))->getZExtValue();
    out.push_back(md);
  } else if (valueTy->isStructTy()) {
    // Structure type's metadata:[{element metadata type}, ...]
    auto elementCount = valueTy->getStructNumElements();
    assert(elementCount == mds->getType()->getStructNumElements());
    for (signed opIdx = 0; opIdx < elementCount; opIdx++) {
      decodeInOutMetaRecursively(valueTy->getStructElementType(opIdx), cast<Constant>(mds->getOperand(opIdx)), out);
    }
  } else {
    llvm_unreachable("The Type can't be handle in decodeInOutMetaRecursively.");
  }
}
} // namespace Llpc
