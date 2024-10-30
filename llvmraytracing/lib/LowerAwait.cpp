/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- LowerAwait.cpp - Replace await calls with coroutine intrinsics -----===//
//
// A pass that introduces coroutine intrinsics. All calls to `await` mark
// a resume point.
//
// This pass introduces a global for the return address, which is saved at the
// start of a function and used in a `@lgc.cps.jump` call in the
// end.
//
//===----------------------------------------------------------------------===//

#include "llvmraytracing/Continuations.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"

using namespace llvm;

#define DEBUG_TYPE "lower-await"

namespace {
class LowerAwaitPassImpl final {
public:
  LowerAwaitPassImpl(Module &Mod);
  PreservedAnalyses run();

private:
  Module &Mod;
  MapVector<Function *, SmallVector<CallInst *>> ToProcess;
  void collectContinuationFunctions();
  void processContinuations();
};
} // anonymous namespace

LowerAwaitPassImpl::LowerAwaitPassImpl(Module &Mod) : Mod{Mod} {
}

void LowerAwaitPassImpl::processContinuations() {
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
  auto &Context = Mod.getContext();
  auto *I8Ptr = Type::getInt8Ty(Context)->getPointerTo();
  auto *I32 = Type::getInt32Ty(Context);

  Type *TokenTy = StructType::create(Context, "continuation.token")->getPointerTo();

  SmallVector<Type *> ReturnTypes;
  ReturnTypes.push_back(I8Ptr);   // Continue function pointer
  ReturnTypes.push_back(TokenTy); // Token to connect the function call with the resume point
  StructType *NewRetTy = StructType::get(Context, ReturnTypes);

  for (auto &FuncData : ToProcess) {
    Function *F = FuncData.first;

    LLVM_DEBUG(dbgs() << "Processing function: " << F->getName() << "\n");

    // Change the return type and arguments
    SmallVector<Type *> AllArgTypes;

    // Lgc.cps dialect will handle stack pointer and return address in
    // DXILContPostProcessPass.
    for (auto const &Arg : F->args())
      AllArgTypes.push_back(Arg.getType());

    // Add new storage pointer for the coroutine passes to new function type at
    // the end
    AllArgTypes.push_back(I8Ptr);

    // Create new empty function
    auto *NewFuncTy = FunctionType::get(NewRetTy, AllArgTypes, false);
    Function *NewFunc = CompilerUtils::cloneFunctionHeader(*F, NewFuncTy, ArrayRef<AttributeSet>{});
    NewFunc->takeName(F);

    // Transfer code from old function to new function
    llvm::moveFunctionBody(*F, *NewFunc);

    for (unsigned Idx = 0; Idx != F->getFunctionType()->params().size(); ++Idx) {
      Argument *Arg = NewFunc->getArg(Idx);
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
        Mod.getOrInsertFunction((Twine("continuation.prototype.") + NewFunc->getName()).toStringRef(StrBuf),
                                FunctionType::get(NewRetTy, {I8Ptr, Type::getInt1Ty(Context)}, false))
            .getCallee());

    // Add metadata, marking it as a continuation function
    MDTuple *ContMDTuple = MDTuple::get(Context, {ValueAsMetadata::get(NewFunc)});
    NewFunc->setMetadata(ContHelper::MDContinuationName, ContMDTuple);
    ContProtoFunc->setMetadata(ContHelper::MDContinuationName, ContMDTuple);

    auto *ContProtoFuncPtr = ConstantExpr::getBitCast(ContProtoFunc, I8Ptr);

    // Alloc and free prototypes too
    auto *ContMallocTy = FunctionType::get(I8Ptr, {I32}, false);
    auto *ContMalloc = dyn_cast<Function>(Mod.getOrInsertFunction("continuation.malloc", ContMallocTy).getCallee());
    auto *ContMallocPtr = ConstantExpr::getBitCast(ContMalloc, I8Ptr);

    auto *ContDeallocTy = FunctionType::get(Type::getVoidTy(Context), {I8Ptr}, false);
    auto *ContDealloc = dyn_cast<Function>(Mod.getOrInsertFunction("continuation.free", ContDeallocTy).getCallee());
    auto *ContDeallocPtr = ConstantExpr::getBitCast(ContDealloc, I8Ptr);

    llvm_dialects::Builder B(&*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    // Claim that the buffer has the minimum required size of a pointer
    Value *BufSize = ConstantInt::get(I32, MinimumContinuationStateBytes);
    Value *BufAlign = ConstantInt::get(I32, 4);

    Value *const CoroId =
        B.CreateIntrinsic(Intrinsic::coro_id_retcon, {},
                          {BufSize, BufAlign, StorageArg, ContProtoFuncPtr, ContMallocPtr, ContDeallocPtr});
    auto *CPN = ConstantPointerNull::get(I8Ptr);
    B.CreateIntrinsic(Intrinsic::coro_begin, {}, {CoroId, CPN});

    // Replace await calls with suspend points
    for (auto *CI : FuncData.second) {
      B.SetInsertPoint(CI);
      Value *SuspendRetconArg = nullptr;
      SmallVector<Value *> Args;
      SmallVector<Type *> ArgTys;
      for (Value *Arg : CI->args()) {
        Args.push_back(Arg);
        ArgTys.push_back(Arg->getType());
      }

      // Insert a dummy call to remember the arguments to lgc.cps.await.
      auto *ShaderTy = FunctionType::get(TokenTy, ArgTys, false);
      auto *ShaderFun = B.CreateIntToPtr(CI->getArgOperand(0), ShaderTy->getPointerTo());
      SuspendRetconArg = B.CreateCall(ShaderTy, ShaderFun, Args);
      cast<CallInst>(SuspendRetconArg)->copyMetadata(*CI);

      B.CreateIntrinsic(Intrinsic::coro_suspend_retcon, {B.getInt1Ty()}, SuspendRetconArg);
      auto *RetTy = CI->getType();
      if (!RetTy->isVoidTy()) {
        auto *RetVal = B.create<lgc::ilcps::GetReturnValueOp>(RetTy);
        CI->replaceAllUsesWith(RetVal);
      }
      CI->eraseFromParent();
    }
  }
}

PreservedAnalyses LowerAwaitPassImpl::run() {
  struct VisitorPayload {
    LowerAwaitPassImpl &Self;
  };

  static auto Visitor = llvm_dialects::VisitorBuilder<VisitorPayload>()
                            .add<lgc::cps::AwaitOp>([](VisitorPayload &Payload, auto &Op) {
                              Payload.Self.ToProcess[Op.getFunction()].push_back(&Op);
                            })
                            .build();

  VisitorPayload P{*this};
  Visitor.visit(P, Mod);

  for (auto &F : Mod) {
    // Force processing annotated functions, even if they don't have await
    // calls
    if (F.hasMetadata(ContHelper::MDContinuationName))
      ToProcess.insert({&F, {}});
  }

  if (!ToProcess.empty()) {
    processContinuations();
    fixupDxilMetadata(Mod);
    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

LowerAwaitPass::LowerAwaitPass() {
}

llvm::PreservedAnalyses LowerAwaitPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the lower-await pass\n");
  AnalysisManager.getResult<DialectContextAnalysis>(M);

  LowerAwaitPassImpl Impl{M};

  return Impl.run();
}
