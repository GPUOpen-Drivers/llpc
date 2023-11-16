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

//= LegacyCleanupContinuations.cpp - Post-process output of coroutine passes =//
//
// Convert the result from the coroutine passes to something more suitable for
// the compiler backend.
//
// Instead of return values, use continue, waitContinue and complete intrinsics.
// Add arguments to resume functions, which are the return values of the called
// continuation.
//
// Add a global register buffer to store the continuation state.
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "continuations/ContinuationsDialect.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/MathExtras.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "legacy-cleanup-continuations"

LegacyCleanupContinuationsPass::LegacyCleanupContinuationsPass() {}

/// Find the original call that created the continuation token and the matching
/// resume function for a return value.
///
/// Returns a map (origin BB, (call that created the continuation token, resume
/// function)).
static DenseMap<BasicBlock *, std::pair<CallInst *, Value *>>
findTokenOrigin(BasicBlock *BB, Value *V,
                SmallVectorImpl<Instruction *> &ToRemove) {
  DenseMap<BasicBlock *, std::pair<CallInst *, Value *>> Result;
  Value *Call = nullptr;
  Value *ResumeFun = nullptr;
  while (auto *Insert = dyn_cast<InsertValueInst>(V)) {
    LLVM_DEBUG(dbgs() << "Insert: " << *Insert << "\n");
    assert(Insert->getNumIndices() == 1 && "Expected a flat struct");
    if (*Insert->idx_begin() == 0)
      ResumeFun = Insert->getInsertedValueOperand();
    else if (*Insert->idx_begin() == 1)
      Call = Insert->getInsertedValueOperand();
    V = Insert->getAggregateOperand();
    ToRemove.push_back(Insert);
  }

  if (!ResumeFun) {
    if (auto *Const = dyn_cast<ConstantStruct>(V))
      ResumeFun = Const->getOperand(0);
  }

  assert(Call && "Did not find call that creates the token");
  assert(ResumeFun && "Did not find resume function");

  // Strip bitcast
  if (auto *Cast = dyn_cast<BitCastInst>(ResumeFun)) {
    ResumeFun = Cast->getOperand(0);
    ToRemove.push_back(Cast);
  }
  if (auto *Const = dyn_cast<ConstantExpr>(ResumeFun)) {
    if (Const->isCast())
      ResumeFun = Const->getOperand(0);
  }

  auto RegisterTokenOrigin = [&Result](BasicBlock *TheBB, Value *Token,
                                       Value *TheResumeFun) {
    assert(isa<Constant>(TheResumeFun) &&
           "Resume function should be a constant function");
    // Strip away bitcasts -- this can happen with multiple token types
    if (auto *TokenBitcast = dyn_cast<BitCastOperator>(Token))
      Token = TokenBitcast->getOperand(0);
    assert(isa<CallInst>(Token) && "Call should be a CallInst");
    auto *CallI = cast<CallInst>(Token);
    Result.insert(std::make_pair(TheBB, std::make_pair(CallI, TheResumeFun)));
  };

  // Walk through phis
  if (auto *CallPhi = dyn_cast<PHINode>(Call)) {
    assert(isa<PHINode>(ResumeFun) && "Resume fun should also be a phi node");
    auto *ResumeFunPhi = cast<PHINode>(ResumeFun);
    ToRemove.push_back(CallPhi);
    ToRemove.push_back(ResumeFunPhi);

    for (auto CallEntry :
         llvm::zip(CallPhi->blocks(), CallPhi->incoming_values())) {
      auto *PhiBB = std::get<0>(CallEntry);
      auto *ResumeFunEntry = ResumeFunPhi->getIncomingValueForBlock(PhiBB);
      assert(ResumeFunEntry && "Need a resume fun for each call");
      RegisterTokenOrigin(PhiBB, std::get<1>(CallEntry), ResumeFunEntry);
    }
  } else {
    RegisterTokenOrigin(BB, Call, ResumeFun);
  }
  return Result;
}

/// Create a memcopy of an array, which the translator understands
static void createCopy(IRBuilder<> &B, Value *Dst, Value *Src, Type *Ty) {
  assert(Ty->isArrayTy() && "Can only copy arrays");
  for (unsigned I = 0; I < Ty->getArrayNumElements(); I++) {
    auto *SrcGep = B.CreateConstInBoundsGEP2_32(Ty, Src, 0, I);
    auto *DstGep = B.CreateConstInBoundsGEP2_32(Ty, Dst, 0, I);
    auto *Load = B.CreateLoad(Ty->getArrayElementType(), SrcGep);
    B.CreateStore(Load, DstGep);
  }
}

void LegacyCleanupContinuationsPass::analyzeContinuation(Function &F,
                                                         MDNode *MD) {
  // Only analyze main continuation
  auto *MDTup = cast<MDTuple>(MD);
  auto *EntryF = mdconst::extract<Function>(MDTup->getOperand(0));

  auto &Data = ToProcess[EntryF];

  if (&F != EntryF) {
    Data.Functions.push_back(&F);
    return;
  }
  Data.Functions.insert(Data.Functions.begin(), &F);
  Data.MD = MD;

  // Search the malloc call to find the size of the continuation state
  if (ContMalloc) {
    for (auto *User : ContMalloc->users()) {
      if (auto *Call = dyn_cast<CallInst>(User)) {
        if (Call->getFunction() == &F) {
          Data.MallocCall = Call;
          break;
        }
      }
    }
  }

  // Without malloc call, we check later if the continuation state is used
  if (Data.MallocCall) {
    Data.ContStateBytes =
        cast<ConstantInt>(Data.MallocCall->getArgOperand(0))->getSExtValue();
  }
  if (Data.ContStateBytes > MaxContStateBytes)
    MaxContStateBytes = Data.ContStateBytes;
}

void LegacyCleanupContinuationsPass::processContinuations() {
  auto &Context = M->getContext();
  auto *Void = Type::getVoidTy(Context);

  for (auto &FuncData : ToProcess) {
    LLVM_DEBUG(dbgs() << "Processing function: " << FuncData.first->getName()
                      << "\n");
    bool IsEntry = FuncData.first->hasMetadata(DXILContHelper::MDEntryName);

    for (auto *F : FuncData.second.Functions) {
      if (F != FuncData.first) {
        // Entry marker should only be on the start and not on resume functions
        F->eraseMetadata(Context.getMDKindID(DXILContHelper::MDEntryName));
        // Same for stacksize
        F->eraseMetadata(Context.getMDKindID(DXILContHelper::MDStackSizeName));
        // Set same linkage as for start function
        F->setLinkage(FuncData.first->getLinkage());
      }

      // Ignore the stub created for the coroutine passes
      if (F->empty())
        continue;

      LLVM_DEBUG(dbgs() << "Processing function part: " << F->getName()
                        << "\n");

      bool IsStart = F == FuncData.first; // If this is the continuation start
      SmallVector<Type *> AllArgTypes;
      SmallVector<Value *> AllArgValues;
      SmallVector<Instruction *> InstsToRemove;
      AttributeList FAttrs = F->getAttributes();
      SmallVector<AttributeSet> ParamAttrs;

      // Use all arguments except the last (pre-allocated buffer for the
      // coroutine passes) for the continuation start
      if (IsStart) {
        unsigned ArgNo = 0;
        assert(F->arg_size() >= 1 &&
               "Entry function has at least one argument");
        for (auto Arg = F->arg_begin(), ArgEnd = F->arg_end() - 1;
             Arg != ArgEnd; Arg++) {
          AllArgTypes.push_back(Arg->getType());
          AllArgValues.push_back(Arg);
          ParamAttrs.push_back(FAttrs.getParamAttrs(ArgNo));
          ArgNo++;
        }
      } else {
        IRBuilder<> B(&*F->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
        AllArgTypes.push_back(
            getContinuationStackOffsetType(Context)); // continuation stack ptr
        AllArgValues.push_back(nullptr);

        // Find arguments from continuation.returnvalue calls
        for (auto &I : F->getEntryBlock()) {
          if (auto *Intr = dyn_cast<continuations::GetReturnValueOp>(&I)) {
            AllArgTypes.push_back(Intr->getType());
            AllArgValues.push_back(Intr);
            InstsToRemove.push_back(Intr);
          }
        }
      }

      // Find the free call if there is one
      if (ContFree) {
        for (auto *User : ContFree->users()) {
          if (auto *Call = dyn_cast<CallInst>(User)) {
            if (Call->getFunction() == F) {
              InstsToRemove.push_back(Call);
              break;
            }
          }
        }
      }

      // Find the continuation state pointer, either returned by the malloc or
      // given as an argument
      Value *ContFrame = nullptr;
      if (FuncData.second.MallocCall) {
        if (IsStart) {
          ContFrame = FuncData.second.MallocCall;
          InstsToRemove.push_back(FuncData.second.MallocCall);

          auto *BufferArg = F->getArg(F->arg_size() - 1);
          auto *User = BufferArg->getUniqueUndroppableUser();
          auto *Cast = dyn_cast<BitCastInst>(User);
          if (Cast)
            User = Cast->getUniqueUndroppableUser();
          auto *Store = cast<StoreInst>(User);
          InstsToRemove.push_back(Store); // Store needs to be eliminated first
          if (Cast)
            InstsToRemove.push_back(Cast);
        } else {
          // Look for the load of the allocated pointer
          auto *User = F->getArg(0)->getUniqueUndroppableUser();
          auto *Cast = dyn_cast<BitCastInst>(User);
          if (Cast)
            User = Cast->getUniqueUndroppableUser();
          auto *Load = cast<LoadInst>(User);
          InstsToRemove.push_back(Load); // Load needs to be eliminated first
          if (Cast)
            InstsToRemove.push_back(Cast);
          ContFrame = Load;
        }
      } else {
        if (IsStart)
          ContFrame = F->getArg(F->arg_size() - 1);
        else
          ContFrame = F->getArg(0);
      }

      // Create new empty function
      F->eraseMetadata(FuncData.second.MD->getMetadataID());
      auto *NewFuncTy = FunctionType::get(Void, AllArgTypes, false);
      Function *NewFunc = cloneFunctionHeader(*F, NewFuncTy, ParamAttrs);
      NewFunc->takeName(F);
      FuncData.second.NewFunctions.push_back(NewFunc);

      // Transfer code from old function to new function
      llvm::moveFunctionBody(*F, *NewFunc);

      // Set arg names for new function
      for (unsigned Idx = 0; Idx != NewFunc->getFunctionType()->params().size();
           ++Idx) {
        Argument *Arg = NewFunc->getArg(Idx);
        Value *OldVal = AllArgValues[Idx];
        if (OldVal) {
          Arg->setName(OldVal->getName());
          OldVal->replaceAllUsesWith(Arg);
        }
        if (IsStart) {
          Argument *OldArg = F->getArg(Idx);
          if (OldArg->hasInRegAttr())
            Arg->addAttr(Attribute::InReg);
          else
            Arg->removeAttr(Attribute::AttrKind::InReg);
        }
      }

      // Handle the function entry
      IRBuilder<> B(&*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
      if (IsStart)
        FuncData.second.NewStart = NewFunc;
      handleFunctionEntry(B, FuncData.second, NewFunc, IsEntry);

      // Handle the function body
      // Use the global continuation state
      ContFrame->replaceAllUsesWith(B.CreateBitOrPointerCast(
          FuncData.second.NewContState, ContFrame->getType()));

      // Handle the function returns
      for (auto &BB : make_early_inc_range(*NewFunc)) {
        auto *I = BB.getTerminator();
        if (I->getOpcode() == Instruction::Ret) {
          handleContinue(B, FuncData.second, I);
        } else if (I->getOpcode() == Instruction::Unreachable) {
          if (auto *Call = dyn_cast<CallInst>(--I->getIterator())) {
            if (auto *Called = Call->getCalledFunction()) {
              if (Called->getName() == "continuation.return")
                handleReturn(B, FuncData.second, Call);
            }
          }
        }
      }

      for (auto *I : InstsToRemove)
        I->eraseFromParent();

      // Remove the old function
      F->replaceAllUsesWith(ConstantExpr::getBitCast(NewFunc, F->getType()));
    }
  }

  // Remove the old functions and update metadata
  for (auto &FuncData : ToProcess) {
    if (FuncData.second.Functions.size() > 1) {
      // Only for functions that were split
      for (auto *F : FuncData.second.Functions)
        F->eraseFromParent();

      MDTuple *ContMDTuple = MDTuple::get(
          Context, {ValueAsMetadata::get(FuncData.second.NewStart)});
      for (auto *F : FuncData.second.NewFunctions) {
        F->setMetadata(DXILContHelper::MDContinuationName, ContMDTuple);
        if (F != FuncData.second.NewStart) {
          // For non-start functions, set (incoming) continuation registercount
          // metadata by looking at the continue calls that reference this
          // function. These continue calls both specify the number of their
          // outgoing registers, and the number of incoming payload registers
          // coming back into the resume function (i.e. us).
          SmallVector<User *> Worklist(F->users());
          std::optional<uint32_t> RegCount;
          while (!Worklist.empty()) {
            auto *U = Worklist.pop_back_val();
            if (auto *Const = dyn_cast<Constant>(U)) {
              Worklist.append(Const->user_begin(), Const->user_end());
              continue;
            }
            assert(isa<CallInst>(U) &&
                   "User of a resume function should be a call to continue");
            auto *Inst = cast<CallInst>(U);
            if (auto Count =
                    DXILContHelper::tryGetReturnedRegisterCount(Inst)) {
              assert((!RegCount || *RegCount == *Count) &&
                     "Got different returned registercounts in continues to "
                     "the same resume function");
              RegCount = *Count;
#ifdef NDEBUG
              break;
#endif
            } else {
              LLVM_DEBUG(Inst->dump());
              report_fatal_error(
                  "Found a continue call without "
                  "continuation returned registercount metadata");
            }
          }

          // Add metadata
          DXILContHelper::setIncomingRegisterCount(F, RegCount.value());
        }
      }
    }
  }

  fixupDxilMetadata(*M);
}

void LegacyCleanupContinuationsPass::handleFunctionEntry(IRBuilder<> &B,
                                                         ContinuationData &Data,
                                                         Function *F,
                                                         bool IsEntry) {
  bool IsStart = F == Data.NewStart;

  // Create alloca to keep the continuation state
  uint64_t ContStateNumI32s = divideCeil(Data.ContStateBytes, RegisterBytes);
  auto *ContStateTy = ArrayType::get(I32, ContStateNumI32s);
  Data.NewContState = B.CreateAlloca(ContStateTy, nullptr, "cont.state");

  uint64_t NeededStackSize = computeNeededStackSizeForRegisterBuffer(
      ContStateNumI32s, ContinuationStateRegisterCount);
  if (IsStart) {
    // Add function metadata that stores how big the continuation state is in
    // bytes
    DXILContHelper::setContinuationStateByteCount(*F, Data.ContStateBytes);

    // Add intrinsic call to save the previous continuation state
    if (!IsEntry && Data.ContStateBytes)
      B.CreateCall(SaveContState);

    if (NeededStackSize) {
      // Add to continuation stack size metadata
      DXILContHelper::addStackSize(F, NeededStackSize);
    }
  } else {
    // Read continuation state from global into local variable
    createCopy(
        B, Data.NewContState,
        B.CreateBitOrPointerCast(
            ContState, ContStateTy->getPointerTo(ContState->getAddressSpace())),
        ContStateTy);

    // Deallocate continuation stack space if necessary
    if (NeededStackSize) {
      // Add barrier so that the csp is only decremented after the continuation
      // state is read
      auto *Csp = B.CreateCall(
          getContinuationStackOffset(*B.GetInsertPoint()->getModule()));
      B.CreateCall(RegisterBufferSetPointerBarrier, {ContState, Csp});

      moveContinuationStackOffset(B, -NeededStackSize);
    }
  }
}

/// Transform
///   %tok = call %continuation.token* @foo() !continuation.registercount !0
///   %0 = insertvalue { i8*, %continuation.token* } { i8* bitcast ({ i8*,
///     %continuation.token* } (i8*, i1)* @fun.resume.0 to i8*),
///     %continuation.token* undef }, %continuation.token* %tok, 1
///   ret { i8*, %continuation.token* } %0
/// to
///   %resume_addr = ptrtoint i8* ... @fun.resume.0 to i64
///   %foo = ptrtoint %continuation.token* () @foo to i64
///   call void @continuation.continue(i64 %foo, i8 addrspace(21)* %csp, i64
///     %resume_addr, <foo args>) !continuation.registercount !0
///   unreachable
///
/// Also handles cases where the token and resume function are behind a phi.
void LegacyCleanupContinuationsPass::handleContinue(IRBuilder<> &B,
                                                    ContinuationData &Data,
                                                    Instruction *Ret) {
  // Find the function call that generates the token
  LLVM_DEBUG(dbgs() << "Converting ret to continue: " << *Ret
                    << "\nArgument: " << *Ret->getOperand(0) << "\n");
  auto *BB = Ret->getParent();
  SmallVector<Instruction *> ToRemove;
  ToRemove.push_back(Ret);
  auto Calls = findTokenOrigin(Ret->getParent(), Ret->getOperand(0), ToRemove);

  for (auto *I : ToRemove)
    I->eraseFromParent();

  for (auto &Entry : Calls) {
    LLVM_DEBUG(dbgs() << "Handling call: " << *Entry.second.first
                      << " with resume function " << Entry.second.second
                      << "\n");
    auto *Call = Entry.second.first;
    auto *ResumeFun = Entry.second.second;
    handleSingleContinue(B, Data, Call, ResumeFun);
  }

  if (BB->empty()) {
    assert(BB->hasNPredecessorsOrMore(0) &&
           "Handled all continues but the block still has predecessors left");
    BB->eraseFromParent();
  }
}

void LegacyCleanupContinuationsPass::handleSingleContinue(
    IRBuilder<> &B, ContinuationData &Data, CallInst *Call, Value *ResumeFun) {
  // Pass resume address as argument
  B.SetInsertPoint(Call);
  auto *ReturnAddrInt = B.CreatePtrToInt(ResumeFun, I64);

  auto *CpsType = getContinuationStackOffsetType(Call->getContext());
  auto *CspFun = getContinuationStackOffset(*Call->getModule());

  // Write local continuation state to stack and registers
  uint64_t ContStateNumI32s = divideCeil(Data.ContStateBytes, RegisterBytes);
  uint64_t NeededStackSize = computeNeededStackSizeForRegisterBuffer(
      ContStateNumI32s, ContinuationStateRegisterCount);

  if (NeededStackSize) {
    // Allocate continuation stack space if necessary
    moveContinuationStackOffset(B, NeededStackSize);
    // Add barrier so that the csp is only incremented before the continuation
    // state is written
    auto *Csp = B.CreateCall(CspFun);
    B.CreateCall(RegisterBufferSetPointerBarrier, {ContState, Csp});
  }

  // Copy continuation state from local variable into global
  auto *ContStateTy = Data.NewContState->getAllocatedType();
  createCopy(
      B,
      B.CreateBitOrPointerCast(
          ContState, ContStateTy->getPointerTo(ContState->getAddressSpace())),
      Data.NewContState, ContStateTy);

  auto *Csp = B.CreateLoad(CpsType, B.CreateCall(CspFun));

  bool IsWait = DXILContHelper::isWaitAwaitCall(*Call);
  Function *ContinueFunction = IsWait ? WaitContinue : Continue;

  // Replace this instruction with a call to continuation.[wait]continue
  SmallVector<Value *> Args;
  Args.push_back(B.CreatePointerCast(Call->getCalledOperand(), I64));
  // The wait mask is the first argument after the function pointer
  if (IsWait)
    Args.push_back(*Call->arg_begin());
  Args.push_back(Csp);
  Args.push_back(ReturnAddrInt);
  Args.append(Call->arg_begin() + (IsWait ? 1 : 0), Call->arg_end());
  auto *ContinueCall = B.CreateCall(ContinueFunction, Args);
  // Copy metadata, except for the wait flag, which is no longer needed.
  ContinueCall->copyMetadata(*Call);
  if (IsWait)
    DXILContHelper::removeIsWaitAwaitMetadata(*ContinueCall);
  assert(DXILContHelper::tryGetOutgoingRegisterCount(ContinueCall) &&
         "Missing registercount metadata!");

  // Remove instructions at the end of the block
  auto *Unreachable = B.CreateUnreachable();
  for (auto &I : make_early_inc_range(reverse(*ContinueCall->getParent()))) {
    if (&I == Unreachable)
      break;
    I.eraseFromParent();
  }
}

/// Transform
///   call void (i64, ...) @continuation.return(i64 %returnaddr, <return value>)
///   unreachable
/// to
///   <decrement CSP>
///   call void @continuation.restore.continuation_state()
///   call void @continuation.continue(i64 %returnaddr, i8 addrspace(21)* %csp,
///     <return value>)
///   unreachable
void LegacyCleanupContinuationsPass::handleReturn(IRBuilder<> &B,
                                                  ContinuationData &Data,
                                                  CallInst *ContRet) {
  LLVM_DEBUG(dbgs() << "Converting return to continue: " << *ContRet << "\n");
  bool IsEntry = isa<UndefValue>(ContRet->getArgOperand(0));
  B.SetInsertPoint(ContRet);
  if (IsEntry) {
    assert(ContRet->arg_size() == 1 &&
           "Entry functions ignore the return value");
    B.CreateCall(Complete);
  } else {
    // Add intrinsic call to restore the previous continuation state
    if (Data.ContStateBytes)
      B.CreateCall(RestoreContState);

    SmallVector<Value *> Args(ContRet->args());
    auto *CspType = getContinuationStackOffsetType(ContRet->getContext());
    auto *CspFun = getContinuationStackOffset(*ContRet->getModule());
    auto *Csp = B.CreateLoad(CspType, B.CreateCall(CspFun));
    Args.insert(Args.begin() + 1, Csp);

    auto *ContinueCall = B.CreateCall(Continue, Args);
    Data.NewReturnContinues.push_back(ContinueCall);

    ContinueCall->copyMetadata(*ContRet);
    assert(DXILContHelper::tryGetOutgoingRegisterCount(ContinueCall) &&
           "Missing registercount metadata!");
  }

  ContRet->eraseFromParent();
}

llvm::PreservedAnalyses LegacyCleanupContinuationsPass::run(
    llvm::Module &Mod, llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the cleanup-continuations pass\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Mod);

  M = &Mod;
  ToProcess.clear();
  MaxContStateBytes = 0;
  ContMalloc = Mod.getFunction("continuation.malloc");
  ContFree = Mod.getFunction("continuation.free");

  // Map the entry function of a continuation to the analysis result
  for (auto &F : Mod.functions()) {
    if (F.empty())
      continue;
    if (auto *MD = F.getMetadata(DXILContHelper::MDContinuationName))
      analyzeContinuation(F, MD);
  }

  // Check if the continuation state is used in any function part
  for (auto &FuncData : ToProcess) {
    if (!FuncData.second.MallocCall) {
      for (auto *F : FuncData.second.Functions) {
        bool IsStart =
            (F == FuncData.first); // If this is the continuation start
        Value *ContFrame;
        if (IsStart)
          ContFrame = F->getArg(F->arg_size() - 1);
        else
          ContFrame = F->getArg(0);
        // If there are uses, we need to assume a size of
        // MinimumContinuationStateBytes, because for all sizes up to this size
        // coroutine passes will not emit a malloc that we can use to determine
        // the exact size. If however the frame pointer is not used in any of
        // the continuation functions, it's safe to assume an empty continuation
        // state.
        if (!ContFrame->user_empty()) {
          FuncData.second.ContStateBytes = MinimumContinuationStateBytes;
          if (MinimumContinuationStateBytes > MaxContStateBytes)
            MaxContStateBytes = MinimumContinuationStateBytes;
        }
      }
    }
  }

  if (!ToProcess.empty()) {
    auto &Context = Mod.getContext();
    I32 = Type::getInt32Ty(Context);
    I64 = Type::getInt64Ty(Context);
    SaveContState = getContinuationSaveContinuationState(Mod);
    RestoreContState = getContinuationRestoreContinuationState(Mod);
    Continue = getContinuationContinue(Mod);
    WaitContinue = getContinuationWaitContinue(Mod);
    Complete = getContinuationComplete(Mod);

    // Add global
    // Size is the maximum of all continuations, but at least the register size
    uint32_t ContStateSize = std::max(
        MaxContStateBytes, ContinuationStateRegisterCount * RegisterBytes);
    auto *ContStateTy =
        ArrayType::get(I32, divideCeil(ContStateSize, RegisterBytes));
    ContState = cast<GlobalVariable>(Mod.getOrInsertGlobal(
        DXILContHelper::GlobalContStateName, ContStateTy, [&] {
          return new GlobalVariable(Mod, ContStateTy, false,
                                    GlobalVariable::ExternalLinkage, nullptr,
                                    DXILContHelper::GlobalContStateName,
                                    nullptr, GlobalVariable::NotThreadLocal);
        }));

    RegisterBufferSetPointerBarrier = getRegisterBufferSetPointerBarrier(Mod);

    // Add registerbuffer metadata to split accesses at into i32s and spill to
    // memory if necessary
    std::optional<ContStackAddrspace> StackAddrspace =
        DXILContHelper::tryGetStackAddrspace(*M);
    if (!StackAddrspace)
      report_fatal_error("Missing stack addrspace metadata!");
    RegisterBufferMD RMD;
    RMD.RegisterCount = ContinuationStateRegisterCount;
    RMD.Addrspace = static_cast<uint32_t>(*StackAddrspace);
    auto *MD = createRegisterBufferMetadata(Context, RMD);
    ContState->addMetadata("registerbuffer", *MD);

    processContinuations();
  }

  if (!ToProcess.empty())
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
