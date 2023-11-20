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

#include "compilerutils/CompilerUtils.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

using namespace llvm;

// =====================================================================================================================
// Create an LLVM function call to the named function. The callee is built
// automatically based on return type and its parameters.
//
// @param funcName : Name of the callee
// @param retTy : Return type of the callee
// @param args : Arguments to pass to the callee
// @param attribs : Function attributes
// @param instName : Name to give instruction
CallInst *CompilerUtils::createNamedCall(IRBuilder<> &builder, StringRef funcName, Type *retTy, ArrayRef<Value *> args,
                                         ArrayRef<Attribute::AttrKind> attribs, const Twine &instName) {
  assert(!funcName.empty());
  Module *mod = builder.GetInsertBlock()->getParent()->getParent();
  Function *func = dyn_cast_or_null<Function>(mod->getFunction(funcName));
  if (!func) {
    SmallVector<Type *, 8> argTys;
    argTys.reserve(args.size());
    for (auto *arg : args)
      argTys.push_back(arg->getType());

    auto *funcTy = FunctionType::get(retTy, argTys, false);
    func = Function::Create(funcTy, GlobalValue::ExternalLinkage, funcName, mod);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::NoUnwind);

    for (auto attrib : attribs) {
      switch (attrib) {
      default:
        func->addFnAttr(attrib);
        break;
      case Attribute::ReadNone:
        func->setDoesNotAccessMemory();
        break;
      case Attribute::ReadOnly:
        func->setOnlyReadsMemory();
        break;
      case Attribute::WriteOnly:
        func->setOnlyWritesMemory();
        break;
      }
    }
  }

  auto *call = builder.CreateCall(func, args, instName);
  call->setCallingConv(CallingConv::C);
  call->setAttributes(func->getAttributes());

  return call;
}

// Modify the function argument types, and return the new function. NOTE: the
// function does not do any uses replacement, so the caller should call
// replaceAllUsesWith() for the function and arguments afterwards.
Function *CompilerUtils::mutateFunctionArguments(Function &fn, Type *retTy, const ArrayRef<Type *> argTys,
                                                 AttributeList attributes) {
  FunctionType *newFnTy = FunctionType::get(retTy, argTys, false);
  auto *newFn = Function::Create(newFnTy, fn.getLinkage());
  newFn->copyAttributesFrom(&fn);
  newFn->copyMetadata(&fn, 0);
  newFn->takeName(&fn);
  newFn->setAttributes(attributes);
  newFn->splice(newFn->begin(), &fn);
  fn.getParent()->getFunctionList().insertAfter(fn.getIterator(), newFn);
  return newFn;
}
