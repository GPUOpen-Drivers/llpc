/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/state/ShaderStage.h"
#include "lgc/state/Abi.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/util/Internal.h"
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
    if (!func.isDeclaration()) {
      if (stage != ShaderStageInvalid)
        func.setMetadata(mdKindId, stageMetaNode);
      else
        func.eraseMetadata(mdKindId);
    }
  }
}

// =====================================================================================================================
// Set shader stage metadata on a function
//
// @param [in/out] func : Function to set shader stage on
// @param stage : Shader stage to set or ShaderStageInvalid
void lgc::setShaderStage(Function *func, ShaderStage stage) {
  unsigned mdKindId = func->getContext().getMDKindID(ShaderStageMetadata);
  if (stage != ShaderStageInvalid) {
    auto stageMetaNode = MDNode::get(
        func->getContext(), {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(func->getContext()), stage))});
    func->setMetadata(mdKindId, stageMetaNode);
  } else
    func->eraseMetadata(mdKindId);
}

// =====================================================================================================================
// Gets the shader stage from the specified LLVM function. Returns ShaderStageInvalid if metadata not found.
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
// Determine whether the function is a shader entry-point.
// A shader entry-point is marked DLLExportStorageClass by markShaderEntryPoint() in Compiler.cpp, which the front-end
// must call before IR linking.
//
// @param func : LLVM function
bool lgc::isShaderEntryPoint(const Function *func) {
  return !func->isDeclaration() && func->getDLLStorageClass() == GlobalValue::DLLExportStorageClass;
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

  static const char *ShaderStageAbbrs[] = {"TASK", "VS", "TCS", "TES", "GS", "MESH", "FS", "CS"};
  return ShaderStageAbbrs[static_cast<unsigned>(shaderStage)];
}

// =====================================================================================================================
// Add args to a function. This creates a new function with the added args, then moves everything from the old function
// across to it.
// If this changes the return type, then all the return instructions will be invalid.
// This does not erase the old function, as the caller needs to do something with its uses (if any).
//
// @param oldFunc : Original function
// @param retTy : New return type, nullptr to use the same as in the original function
// @param argTys : Types of new args
// @param inRegMask : Bitmask of which args should be marked "inreg", to be passed in SGPRs
// @param append : Append new arguments if true, prepend new arguments if false
// @returns : The new function
Function *lgc::addFunctionArgs(Function *oldFunc, Type *retTy, ArrayRef<Type *> argTys, ArrayRef<std::string> argNames,
                               uint64_t inRegMask, bool append) {
  // Gather all arg types: first the new ones, then the ones from the original function.
  FunctionType *oldFuncTy = oldFunc->getFunctionType();
  SmallVector<Type *, 8> allArgTys;
  // Old arguments first if new arguments are appended.
  if (append)
    allArgTys.append(oldFuncTy->params().begin(), oldFuncTy->params().end());
  allArgTys.append(argTys.begin(), argTys.end());
  // Old arguments last if new arguments are prepended.
  if (!append)
    allArgTys.append(oldFuncTy->params().begin(), oldFuncTy->params().end());

  // Create new empty function.
  if (!retTy)
    retTy = oldFuncTy->getReturnType();
  auto newFuncTy = FunctionType::get(retTy, allArgTys, false);
  Function *newFunc = Function::Create(newFuncTy, oldFunc->getLinkage());
  newFunc->setCallingConv(oldFunc->getCallingConv());
  newFunc->takeName(oldFunc);
  newFunc->setSubprogram(oldFunc->getSubprogram());
  newFunc->setDLLStorageClass(oldFunc->getDLLStorageClass());
  // Always insert the new function after the old function
  oldFunc->getParent()->getFunctionList().insertAfter(oldFunc->getIterator(), newFunc);

  // Transfer code from old function to new function.
  while (!oldFunc->empty()) {
    BasicBlock *block = &oldFunc->front();
    block->removeFromParent();
    block->insertInto(newFunc);
  }

  // Copy attributes and shader stage from the old function. The new arguments have InReg set iff the corresponding
  // bit is set in inRegMask.
  AttributeList oldAttrList = oldFunc->getAttributes();
  SmallVector<AttributeSet, 8> argAttrs;
  if (append) {
    // Old arguments first.
    for (unsigned idx = 0; idx != oldFuncTy->getNumParams(); ++idx)
      argAttrs.push_back(oldAttrList.getParamAttrs(idx));
  }

  // New arguments.
  AttributeSet emptyAttrSet;
  AttributeSet inRegAttrSet = emptyAttrSet.addAttribute(oldFunc->getContext(), Attribute::InReg);
  for (unsigned idx = 0; idx != argTys.size(); ++idx)
    argAttrs.push_back((inRegMask >> idx) & 1 ? inRegAttrSet : emptyAttrSet);
  if (!append) {
    // Old arguments.
    for (unsigned idx = 0; idx != argTys.size(); ++idx)
      argAttrs.push_back(oldAttrList.getParamAttrs(idx));
  }
  // Construct new AttributeList and set it on the new function.
  newFunc->setAttributes(
      AttributeList::get(oldFunc->getContext(), oldAttrList.getFnAttrs(), oldAttrList.getRetAttrs(), argAttrs));

  // Set the shader stage on the new function (implemented with IR metadata).
  setShaderStage(newFunc, getShaderStage(oldFunc));

  // Replace uses of the old args.
  // Set inreg attributes correctly. We have to use removeAttr because arg attributes are actually attached
  // to the Function, and the attribute copy above copied the arg attributes at their original arg numbers.
  // Also set name of each new arg that comes from old arg.
  for (unsigned idx = 0; idx != argTys.size(); ++idx) {
    Argument *arg = newFunc->getArg(append ? idx + oldFuncTy->getNumParams() : idx);
    arg->setName(argNames[idx]);
    if ((inRegMask >> idx) & 1)
      arg->addAttr(Attribute::InReg);
    else if (!oldFuncTy->params().empty())
      arg->removeAttr(Attribute::AttrKind::InReg);
  }
  for (unsigned idx = 0; idx != oldFuncTy->params().size(); ++idx) {
    Argument *arg = newFunc->getArg(append ? idx : idx + argTys.size());
    Argument *oldArg = oldFunc->getArg(idx);
    arg->setName(oldArg->getName());
    oldArg->replaceAllUsesWith(arg);
    if (oldArg->hasInRegAttr())
      arg->addAttr(Attribute::InReg);
    else
      arg->removeAttr(Attribute::AttrKind::InReg);
  }

  return newFunc;
}

// =====================================================================================================================
// Get the ABI-mandated entry-point name for a shader stage
//
// @param callingConv : Which hardware shader stage
// @param isFetchlessVs : Whether it is (or contains) a fetchless vertex shader
// @returns : The entry-point name, or "" if callingConv not recognized
StringRef lgc::getEntryPointName(unsigned callingConv, bool isFetchlessVs) {
  StringRef entryName;
  switch (callingConv) {
  case CallingConv::AMDGPU_CS:
    entryName = Util::Abi::AmdGpuCsEntryName;
    break;
  case CallingConv::AMDGPU_PS:
    entryName = Util::Abi::AmdGpuPsEntryName;
    break;
  case CallingConv::AMDGPU_VS:
    entryName = Util::Abi::AmdGpuVsEntryName;
    if (isFetchlessVs)
      entryName = FetchlessVsEntryName;
    break;
  case CallingConv::AMDGPU_GS:
    entryName = Util::Abi::AmdGpuGsEntryName;
    if (isFetchlessVs)
      entryName = FetchlessGsEntryName;
    break;
  case CallingConv::AMDGPU_ES:
    entryName = Util::Abi::AmdGpuEsEntryName;
    if (isFetchlessVs)
      entryName = FetchlessEsEntryName;
    break;
  case CallingConv::AMDGPU_HS:
    entryName = Util::Abi::AmdGpuHsEntryName;
    if (isFetchlessVs)
      entryName = FetchlessHsEntryName;
    break;
  case CallingConv::AMDGPU_LS:
    entryName = Util::Abi::AmdGpuLsEntryName;
    if (isFetchlessVs)
      entryName = FetchlessLsEntryName;
    break;
  default:
    break;
  }
  return entryName;
}
