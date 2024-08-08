/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// Promotion of pointer args to by-value.

#include "compilerutils/ArgPromotion.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/TypesMetadata.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

static Function *cloneFunctionHeaderWithTypes(Function &F, TypedFuncTy NewType, AttributeList FnAttr) {
  FunctionType *FuncTy = NewType.asFunctionType();
  Function *NewFunc = CompilerUtils::cloneFunctionHeader(F, FuncTy, FnAttr);
  NewType.writeMetadata(NewFunc);
  return NewFunc;
}

/// Copy the function body from the old function.
static Function *cloneFunctionWithTypes(Function *Fn, TypedFuncTy NewFnTy, AttributeList FnAttrs) {
  // Erase outdated types metadata to avoid being propagated to the new
  // function.
  Fn->eraseMetadata(Fn->getContext().getMDKindID(TypedFuncTy::MDTypesName));
  Function *NewFn = cloneFunctionHeaderWithTypes(*Fn, NewFnTy, FnAttrs);
  NewFn->splice(NewFn->begin(), Fn);
  NewFn->takeName(Fn);
  Fn->replaceAllUsesWith(ConstantExpr::getBitCast(NewFn, Fn->getType()));
  return NewFn;
}

/// Unpack the return (struct) type of the input function, which means change
/// the return type to its first element type. This may generate invalid IR in
/// general, call this with extra caution.
Function *CompilerUtils::unpackStructReturnType(Function *Fn) {
  auto *RetTy = Fn->getReturnType();
  assert(RetTy->isStructTy());
  auto *NewRetTy = RetTy->getStructElementType(0);

  SmallVector<TypedArgTy> ArgTys;
  TypedFuncTy::get(Fn).getParamTypes(ArgTys);
  TypedFuncTy NewFnTy(NewRetTy, ArgTys);
  auto *NewFn = cloneFunctionWithTypes(Fn, NewFnTy, Fn->getAttributes());
  llvm::forEachCall(*NewFn, [&](CallInst &Call) {
    // Update callee function type.
    Call.setCalledFunction(NewFn);
  });

  // Copy argument names and replace argument uses.
  for (const auto &[OldArg, NewArg] : llvm::zip(Fn->args(), NewFn->args())) {
    NewArg.setName(OldArg.getName());
    if (!NewFn->isDeclaration())
      OldArg.replaceAllUsesWith(&NewArg);
  }
  IRBuilder<> B(Fn->getContext());
  llvm::forEachTerminator(NewFn, {Instruction::Ret}, [&](Instruction &Terminator) {
    B.SetInsertPoint(&Terminator);
    Value *RetExtractVal = B.CreateExtractValue(Terminator.getOperand(0), {0});
    B.CreateRet(RetExtractVal);
    Terminator.eraseFromParent();
  });
  Fn->eraseFromParent();
  return NewFn;
}

// Turn `StructRet` argument into more canonical return statement.
Function *CompilerUtils::lowerStructRetArgument(Function *Fn) {
  assert(Fn->getReturnType()->isVoidTy());
  auto *RetArg = Fn->getArg(0);
  if (!RetArg->hasStructRetAttr())
    RetArg = Fn->getArg(1);
  assert(RetArg->hasStructRetAttr());
  unsigned RetArgIdx = RetArg->getArgNo();
  Type *RetTy = RetArg->getParamStructRetType();

  AttributeList FnAttrs = Fn->getAttributes();
  SmallVector<AttributeSet> ArgAttrs;
  SmallVector<TypedArgTy> NewArgTys;
  SmallVector<TypedArgTy> OldArgTys;
  TypedFuncTy::get(Fn).getParamTypes(OldArgTys);
  for (unsigned Idx = 0; Idx < Fn->arg_size(); Idx++) {
    if (Idx != RetArgIdx) {
      ArgAttrs.push_back(FnAttrs.getParamAttrs(Idx));
      NewArgTys.push_back(OldArgTys[Idx]);
    }
  }

  TypedFuncTy NewFnTy(RetTy, NewArgTys);
  auto NewFnAttr = AttributeList::get(Fn->getContext(), FnAttrs.getFnAttrs(), FnAttrs.getRetAttrs(), ArgAttrs);
  Function *NewFn = cloneFunctionWithTypes(Fn, NewFnTy, NewFnAttr);

  IRBuilder<> B(Fn->getContext());
  llvm::forEachCall(*NewFn, [&](CallInst &Call) {
    B.SetInsertPoint(&Call);
    Value *StructRetArg = nullptr;
    SmallVector<Value *> Args;
    for (const auto &[Idx, Arg] : llvm::enumerate(Call.args())) {
      if (Idx == RetArgIdx) {
        StructRetArg = Arg;
        continue;
      }
      Args.push_back(Arg);
    }
    auto *NewRet = B.CreateCall(NewFn, Args);
    B.CreateStore(NewRet, StructRetArg);
    Call.eraseFromParent();
  });

  // Copy argument names and replace argument uses.
  for (const auto &[ArgNo, NewArg] : llvm::enumerate(NewFn->args())) {
    auto *OldArg = Fn->getArg(ArgNo >= RetArgIdx ? ArgNo + 1 : ArgNo);
    NewArg.setName(OldArg->getName());
    if (!NewFn->isDeclaration())
      OldArg->replaceAllUsesWith(&NewArg);
  }

  if (!NewFn->isDeclaration()) {
    B.SetInsertPointPastAllocas(NewFn);
    auto *RetAlloca = B.CreateAlloca(RetTy);
    RetArg->replaceAllUsesWith(RetAlloca);

    // Replace returns with return value
    llvm::forEachTerminator(NewFn, {Instruction::Ret}, [&](Instruction &Terminator) {
      B.SetInsertPoint(&Terminator);
      Value *RetLoad = B.CreateLoad(RetTy, RetAlloca);
      B.CreateRet(RetLoad);
      Terminator.eraseFromParent();
    });
  }
  Fn->eraseFromParent();
  return NewFn;
}

/// Promote pointer argument type to its value type if the corresponding bit in
/// `PromotionMask` is being set.
Function *CompilerUtils::promotePointerArguments(Function *Fn, const SmallBitVector &PromotionMask) {
  SmallVector<TypedArgTy> ArgTys;
  SmallVector<AttributeSet> ParamAttrs;

  // Do nothing if the promotion mask is zero.
  if (PromotionMask.none())
    return Fn;

  auto FnAttrs = Fn->getAttributes();
  // The function might not have types metadata, in that
  // case nothing needs to be done.
  if (!Fn->getMetadata(TypedFuncTy::MDTypesName))
    return Fn;

  for (const auto &[ArgNo, Arg] : llvm::enumerate(Fn->args())) {
    TypedArgTy ArgTy = TypedArgTy::get(&Arg);

    // Promote the pointer type to its value type if the bit in `PromotionMask`
    // is set.
    if (PromotionMask[ArgNo]) {
      assert(ArgTy.isPointerTy());
      ArgTys.push_back(ArgTy.getPointerElementType());
      ParamAttrs.push_back({});
      continue;
    }
    ArgTys.push_back(ArgTy);
    ParamAttrs.push_back(FnAttrs.getParamAttrs(ArgNo));
  }

  TypedFuncTy NewFuncTy(TypedFuncTy::get(Fn).getReturnType(), ArgTys);
  auto NewFnAttr = AttributeList::get(Fn->getContext(), FnAttrs.getFnAttrs(), FnAttrs.getRetAttrs(), ParamAttrs);
  auto *NewFn = cloneFunctionWithTypes(Fn, NewFuncTy, NewFnAttr);

  IRBuilder<> B(Fn->getContext());
  // Change argument types at call sites.
  llvm::forEachCall(*NewFn, [&](CallInst &Call) {
    B.SetInsertPoint(&Call);
    for (const auto &[ArgNo, ArgPair] : llvm::enumerate(llvm::zip(Call.args(), NewFn->args()))) {
      auto &CallArg = std::get<0>(ArgPair);
      auto &NewArg = std::get<1>(ArgPair);
      if (CallArg->getType() != NewArg.getType()) {
        auto *NewOp = B.CreateLoad(NewArg.getType(), CallArg);
        Call.setArgOperand(ArgNo, NewOp);
      }
    }
    // Update Callee function type.
    Call.setCalledFunction(NewFn);
  });

  // Replace argument uses.
  for (const auto &[OldArg, NewArg] : llvm::zip(Fn->args(), NewFn->args())) {
    Value *NewValue = &NewArg;
    NewArg.setName(OldArg.getName());
    if (!NewFn->isDeclaration()) {
      if (NewArg.getType() != OldArg.getType()) {
        B.SetInsertPointPastAllocas(NewFn);
        auto *ArgAlloca = B.CreateAlloca(NewArg.getType());
        B.CreateStore(&NewArg, ArgAlloca);
        NewValue = ArgAlloca;
      }
      OldArg.replaceAllUsesWith(NewValue);
    }
  }
  Fn->eraseFromParent();
  return NewFn;
}
