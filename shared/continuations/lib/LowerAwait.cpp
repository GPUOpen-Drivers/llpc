/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *deal in the Software without restriction, including without limitation the
 *rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *IN THE SOFTWARE.
 *
 **********************************************************************************************************************/

//===- LowerAwait.cpp - Replace await calls with coroutine intrinsics -----===//
//
// A pass that introduces coroutine intrinsics. All calls to `await` mark
// a resume point.
//
// This pass introduces a global for the return address, which is saved at the
// start of a function and used in a `@continuation.return(i64)` call in the
// end.
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "continuations/ContinuationsDialect.h"
#include "lgc/LgcCpsDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm-dialects/Dialect/Dialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "lower-await"

Function *llvm::getContinuationSaveContinuationState(Module &M) {
  auto *Name = "continuation.save.continuation_state";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  AttributeList AL = AttributeList::get(
      C, AttributeList::FunctionIndex,
      {Attribute::NoFree, Attribute::NoRecurse, Attribute::NoSync,
       Attribute::NoUnwind, Attribute::WillReturn});
  return cast<Function>(M.getOrInsertFunction(Name, AL, Void).getCallee());
}

Function *llvm::getContinuationRestoreContinuationState(Module &M) {
  auto *Name = "continuation.restore.continuation_state";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  AttributeList AL = AttributeList::get(
      C, AttributeList::FunctionIndex,
      {Attribute::NoFree, Attribute::NoRecurse, Attribute::NoSync,
       Attribute::NoUnwind, Attribute::WillReturn});
  return cast<Function>(M.getOrInsertFunction(Name, AL, Void).getCallee());
}

Function *llvm::getContinuationContinue(Module &M) {
  auto *Name = "continuation.continue";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  auto *I64 = Type::getInt64Ty(C);
  auto *FuncTy = FunctionType::get(Void, {I64}, true);
  AttributeList AL = AttributeList::get(C, AttributeList::FunctionIndex,
                                        {Attribute::NoReturn});
  return cast<Function>(M.getOrInsertFunction(Name, FuncTy, AL).getCallee());
}

Function *llvm::getContinuationWaitContinue(Module &M) {
  auto *Name = "continuation.waitContinue";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  auto *I64 = Type::getInt64Ty(C);
  auto *FuncTy = FunctionType::get(Void, {I64, I64}, true);
  AttributeList AL = AttributeList::get(C, AttributeList::FunctionIndex,
                                        {Attribute::NoReturn});
  return cast<Function>(M.getOrInsertFunction(Name, FuncTy, AL).getCallee());
}

Function *llvm::getContinuationComplete(Module &M) {
  auto *Name = "continuation.complete";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  AttributeList AL = AttributeList::get(C, AttributeList::FunctionIndex,
                                        {Attribute::NoReturn});
  return cast<Function>(M.getOrInsertFunction(Name, AL, Void).getCallee());
}

Function *llvm::getContinuationAwait(Module &M, Type *TokenTy,
                                     StructType *RetTy) {
  std::string Name = "await.";
  Name += RetTy->getStructName();
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  AttributeList AL =
      AttributeList::get(C, AttributeList::FunctionIndex,
                         {Attribute::NoUnwind, Attribute::WillReturn});
  return cast<Function>(
      M.getOrInsertFunction(Name, AL, RetTy, TokenTy).getCallee());
}

Function *llvm::getContinuationCspInit(Module &M) {
  auto *Name = "continuation.initialContinuationStackPtr";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  AttributeList AL =
      AttributeList::get(C, AttributeList::FunctionIndex,
                         {Attribute::NoFree, Attribute::NoRecurse,
                          Attribute::NoUnwind, Attribute::WillReturn});
  return cast<Function>(
      M.getOrInsertFunction(Name, AL, getContinuationStackOffsetType(C))
          .getCallee());
}

static Function *getContinuationReturn(Module &M) {
  auto *Name = "continuation.return";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *Void = Type::getVoidTy(C);
  auto *FuncTy = FunctionType::get(Void, {}, true);
  AttributeList AL = AttributeList::get(C, AttributeList::FunctionIndex,
                                        {Attribute::NoReturn});
  return cast<Function>(M.getOrInsertFunction(Name, FuncTy, AL).getCallee());
}

LowerAwaitPass::LowerAwaitPass() {}

static void processContinuations(
    Module &M, const MapVector<Function *, SmallVector<CallInst *>> &ToProcess,
    bool LowerLgcAwait) {
  // We definitely have a call that requires continuation in this function
  //
  // If this is the first time we've done this for this function
  //   Insert the required calls at the start of the function:
  //       id     = llvm.coro.id.retcon
  //       handle = llvm.coro.begin id
  //   Change the return type of the function to the await token
  // Replace the call with
  //    co.flag = llvm.coro.suspend.retcon
  //       unreachable
  auto &Context = M.getContext();
  auto *I8Ptr = Type::getInt8Ty(Context)->getPointerTo();
  auto *I32 = Type::getInt32Ty(Context);
  auto *I64 = Type::getInt64Ty(Context);

  Type *TokenTy =
      StructType::create(Context, "continuation.token")->getPointerTo();

  SmallVector<Type *> ReturnTypes;
  ReturnTypes.push_back(I8Ptr); // Continue function pointer
  ReturnTypes.push_back(
      TokenTy); // Token to connect the function call with the resume point
  StructType *NewRetTy = StructType::get(Context, ReturnTypes);

  for (auto &FuncData : ToProcess) {
    Function *F = FuncData.first;

    LLVM_DEBUG(dbgs() << "Processing function: " << F->getName() << "\n");

    // Change the return type and arguments
    SmallVector<Type *> AllArgTypes;

    // Lgc.cps dialect will handle stack pointer and return address in other
    // places.
    bool IsLegacyNonEntry =
        !F->hasMetadata(DXILContHelper::MDEntryName) && !LowerLgcAwait;
    // Add continuation stack pointer and passed return address.
    if (IsLegacyNonEntry) {
      AllArgTypes.push_back(getContinuationStackOffsetType(Context));
      AllArgTypes.push_back(I64);
    }

    for (auto const &Arg : F->args())
      AllArgTypes.push_back(Arg.getType());

    // Add new storage pointer for the coroutine passes to new function type at
    // the end
    AllArgTypes.push_back(I8Ptr);

    // Create new empty function
    auto *NewFuncTy = FunctionType::get(NewRetTy, AllArgTypes, false);
    Function *NewFunc = cloneFunctionHeader(*F, NewFuncTy, {});
    NewFunc->takeName(F);

    // Transfer code from old function to new function
    llvm::moveFunctionBody(*F, *NewFunc);

    // Set arg names for new function
    if (IsLegacyNonEntry) {
      NewFunc->getArg(0)->setName("cspInit");
      NewFunc->getArg(1)->setName("returnAddr");
    }
    for (unsigned Idx = 0; Idx != F->getFunctionType()->params().size();
         ++Idx) {
      Argument *Arg = NewFunc->getArg(Idx + (IsLegacyNonEntry ? 2 : 0));
      Argument *OldArg = F->getArg(Idx);
      Arg->setName(OldArg->getName());
      OldArg->replaceAllUsesWith(Arg);
      if (OldArg->hasInRegAttr())
        Arg->addAttr(Attribute::InReg);
      else
        Arg->removeAttr(Attribute::AttrKind::InReg);
    }

    Value *StorageArg = NewFunc->getArg(AllArgTypes.size() - 1);

    // Remove the old function
    F->replaceAllUsesWith(ConstantExpr::getBitCast(NewFunc, F->getType()));
    F->eraseFromParent();

    // Create the continuation prototype function
    // We need one per continuation because they have different metadata
    SmallVector<char> StrBuf;
    auto *ContProtoFunc = cast<Function>(
        M.getOrInsertFunction(
             (Twine("continuation.prototype.") + NewFunc->getName())
                 .toStringRef(StrBuf),
             FunctionType::get(NewRetTy, {I8Ptr, Type::getInt1Ty(Context)},
                               false))
            .getCallee());

    // Add metadata, marking it as a continuation function
    MDTuple *ContMDTuple =
        MDTuple::get(Context, {ValueAsMetadata::get(NewFunc)});
    NewFunc->setMetadata(DXILContHelper::MDContinuationName, ContMDTuple);
    ContProtoFunc->setMetadata(DXILContHelper::MDContinuationName, ContMDTuple);

    auto *ContProtoFuncPtr = ConstantExpr::getBitCast(ContProtoFunc, I8Ptr);

    // Alloc and free prototypes too
    auto *ContMallocTy = FunctionType::get(I8Ptr, {I32}, false);
    auto *ContMalloc = dyn_cast<Function>(
        M.getOrInsertFunction("continuation.malloc", ContMallocTy).getCallee());
    auto *ContMallocPtr = ConstantExpr::getBitCast(ContMalloc, I8Ptr);

    auto *ContDeallocTy =
        FunctionType::get(Type::getVoidTy(Context), {I8Ptr}, false);
    auto *ContDealloc = dyn_cast<Function>(
        M.getOrInsertFunction("continuation.free", ContDeallocTy).getCallee());
    auto *ContDeallocPtr = ConstantExpr::getBitCast(ContDealloc, I8Ptr);

    llvm_dialects::Builder B(
        &*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    // Claim that the buffer has the minimum required size of a pointer
    Value *BufSize = ConstantInt::get(I32, MinimumContinuationStateBytes);
    Value *BufAlign = ConstantInt::get(I32, 4);

    Value *const CoroId =
        B.CreateIntrinsic(Intrinsic::coro_id_retcon, {},
                          {BufSize, BufAlign, StorageArg, ContProtoFuncPtr,
                           ContMallocPtr, ContDeallocPtr});
    auto *CPN = ConstantPointerNull::get(I8Ptr);
    B.CreateIntrinsic(Intrinsic::coro_begin, {}, {CoroId, CPN});

    // Replace await calls with suspend points
    for (auto *CI : FuncData.second) {
      B.SetInsertPoint(CI);
      Value *SuspendRetconArg = nullptr;
      if (LowerLgcAwait) {
        SmallVector<Value *> Args;
        SmallVector<Type *> ArgTys;
        for (Value *Arg : CI->args()) {
          Args.push_back(Arg);
          ArgTys.push_back(Arg->getType());
        }

        // Insert a dummy call to remember the arguments to lgc.cps.await.
        auto *ShaderTy = FunctionType::get(TokenTy, ArgTys, false);
        auto *ShaderFun =
            B.CreateIntToPtr(CI->getArgOperand(0), ShaderTy->getPointerTo());
        SuspendRetconArg = B.CreateCall(ShaderTy, ShaderFun, Args);
      } else {
        SuspendRetconArg = CI->getArgOperand(0);
      }
      B.CreateIntrinsic(Intrinsic::coro_suspend_retcon, {B.getInt1Ty()},
                        SuspendRetconArg);
      auto *RetTy = CI->getType();
      if (!RetTy->isVoidTy()) {
        auto *RetVal = B.create<continuations::GetReturnValueOp>(RetTy);
        CI->replaceAllUsesWith(RetVal);
      }
      CI->eraseFromParent();
    }

    // Save the return address at the start of the function for legacy path.
    // For lgc.cps, we don't need to save any value, so just not passing any
    // argument.
    Value *SavedRetAddr = nullptr;
    if (!LowerLgcAwait) {
      if (IsLegacyNonEntry)
        SavedRetAddr = NewFunc->getArg(1); // Return addr
      else
        SavedRetAddr = UndefValue::get(I64);
    }
    // Convert returns to continuation.return calls
    auto *ContRet = getContinuationReturn(M);
    for (auto &BB : *NewFunc) {
      auto *I = BB.getTerminator();
      if (I->getOpcode() == Instruction::Ret) {
        // Replace this instruction with a call to continuation.return
        B.SetInsertPoint(I);
        SmallVector<Value *, 2> RetVals;

        if (!LowerLgcAwait) {
          RetVals.push_back(SavedRetAddr);
          if (I->getNumOperands() != 0)
            RetVals.push_back(I->getOperand(0));
        }
        auto *ContRetCall = B.CreateCall(ContRet, RetVals);
        // DXILCont passes use annotations on the ret to pass information
        // on the shader exit to later passes. Copy such metadata to the ContRet
        // so later passes can pick it up from there.
        ContRetCall->copyMetadata(*I);
        B.CreateUnreachable();
        I->eraseFromParent();
      }
    }
  }
  fixupDxilMetadata(M);
}

llvm::PreservedAnalyses
LowerAwaitPass::run(llvm::Module &M,
                    llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the lower-await pass\n");
  AnalysisManager.getResult<DialectContextAnalysis>(M);

  MapVector<Function *, SmallVector<CallInst *>> ToProcess;
  static auto Visitor =
      llvm_dialects::VisitorBuilder<
          MapVector<Function *, SmallVector<CallInst *>>>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
          .add<lgc::cps::AwaitOp>([](auto &ToProcess, auto &Op) {
            ToProcess[Op.getFunction()].push_back(&Op);
          })
          .build();
  Visitor.visit(ToProcess, M);

  bool LowerLgcAwait = !ToProcess.empty();
  if (!LowerLgcAwait) {
    for (auto &F : M.functions()) {
      if (!F.getName().startswith("await.")) {
        // Force processing annotated functions, even if they don't have await
        // calls
        if (F.hasMetadata(DXILContHelper::MDContinuationName))
          ToProcess[&F].size();
        continue;
      }
      for (auto *U : F.users()) {
        if (auto *Inst = dyn_cast<CallInst>(U))
          ToProcess[Inst->getFunction()].push_back(Inst);
      }
    }
  }

  if (!ToProcess.empty()) {
    processContinuations(M, ToProcess, LowerLgcAwait);
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}
