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

#include "compilerutils/CompilerUtils.h"
#include "continuations/Continuations.h"
#include "continuations/ContinuationsDialect.h"
#include "lgc/LgcRtDialect.h"
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

namespace {

class LegacyCleanupContinuationsPassImpl {
public:
  LegacyCleanupContinuationsPassImpl(
      llvm::Module &Mod, llvm::Module *GpurtLibrary,
      llvm::ModuleAnalysisManager &AnalysisManager);

  llvm::PreservedAnalyses run();

private:
  struct ContinuationData {
    /// All functions belonging to this continuation, the entry function is the
    /// first one
    SmallVector<Function *> Functions;
    /// Size of the continuation state in byte
    uint32_t ContStateBytes = 0;
    CallInst *MallocCall = nullptr;
    MDNode *MD = nullptr;
    AllocaInst *NewContState = nullptr;
    SmallVector<CallInst *> NewReturnContinues;
    /// Cleaned entry function, used to replace metadata
    Function *NewStart = nullptr;

    // Returns the number of bytes used on the CPS stack for the continuation
    // state.
    uint32_t getContStateStackBytes() const {
      return alignTo(ContStateBytes, RegisterBytes);
    }
  };

  void analyzeContinuation(Function &F, MDNode *MD);
  // Run analysis parts that need to wait until all resume functions have been
  // collected
  void finalizeContinuationData(Function &StartFunc, ContinuationData &Data);
  void processContinuation(Function *StartFunc, ContinuationData &FuncData);
  void handleFunctionEntry(ContinuationData &Data, Function *F, bool IsEntry);
  void handleContinue(ContinuationData &Data, Instruction *Ret);
  void handleSingleContinue(ContinuationData &Data, CallInst *Call,
                            Value *ResumeFun);
  void handleReturn(ContinuationData &Data, CallInst *ContRet);

  Module &M;
  LLVMContext &Context;
  IRBuilder<> B;
  Type *I32 = nullptr;
  Type *I64 = nullptr;
  Function *ContMalloc = nullptr;
  Function *ContFree = nullptr;
  Function *Continue = nullptr;
  Function *WaitContinue = nullptr;
  Function *Complete = nullptr;
  MapVector<Function *, ContinuationData> ToProcess;
  uint32_t MaxContStateBytes = 0;
  llvm::Module *GpurtLibrary = nullptr;
  CompilerUtils::CrossModuleInliner CrossInliner;
};

/// Find the original call that created the continuation token and the matching
/// resume function for a return value.
///
/// Returns a map (origin BB, (call that created the continuation token, resume
/// function)).
DenseMap<BasicBlock *, std::pair<CallInst *, Value *>>
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
void createCopy(IRBuilder<> &B, Value *Dst, Value *Src, Type *Ty) {
  assert(Ty->isArrayTy() && "Can only copy arrays");
  for (unsigned I = 0; I < Ty->getArrayNumElements(); I++) {
    auto *SrcGep = B.CreateConstInBoundsGEP2_32(Ty, Src, 0, I);
    auto *DstGep = B.CreateConstInBoundsGEP2_32(Ty, Dst, 0, I);
    auto *Load = B.CreateLoad(Ty->getArrayElementType(), SrcGep);
    B.CreateStore(Load, DstGep);
  }
}

void LegacyCleanupContinuationsPassImpl::analyzeContinuation(Function &F,
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
    forEachCall(*ContMalloc, [&](CallInst &Call) {
      if (Call.getFunction() == &F) {
        Data.MallocCall = &Call;
      }
    });
  }

  // Without malloc call, we check later if the continuation state is used
  if (Data.MallocCall) {
    Data.ContStateBytes =
        cast<ConstantInt>(Data.MallocCall->getArgOperand(0))->getSExtValue();
  }
}

void LegacyCleanupContinuationsPassImpl::finalizeContinuationData(
    Function &StartFunc, ContinuationData &FuncData) {
  if (FuncData.MallocCall)
    return;
  for (auto *F : FuncData.Functions) {
    bool IsStart = (F == &StartFunc); // If this is the continuation start
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
      assert(FuncData.ContStateBytes == 0);
      FuncData.ContStateBytes = MinimumContinuationStateBytes;
      break;
    }
  }
}

// For a resume function, find the continue call to it (by looking at its uses)
// and obtain the incoming payload register count into the resume function
// as the outgoing register count of the continue call, indicated by metadata.
uint32_t getIncomingRegisterCount(Function *ResumeFunc) {
  // For non-start functions, set (incoming) continuation registercount
  // metadata by looking at the continue calls that reference this
  // function. These continue calls both specify the number of their
  // outgoing registers, and the number of incoming payload registers
  // coming back into the resume function (i.e. us).
  SmallVector<User *> Worklist(ResumeFunc->users());
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
    if (auto Count = DXILContHelper::tryGetReturnedRegisterCount(Inst)) {
      assert((!RegCount || *RegCount == *Count) &&
             "Got different returned registercounts in continues to "
             "the same resume function");
      RegCount = *Count;
#ifdef NDEBUG
      break;
#endif
    } else {
      LLVM_DEBUG(Inst->dump());
      report_fatal_error("Found a continue call without "
                         "continuation returned registercount metadata");
    }
  }
  return RegCount.value();
}

Value *getContFrame(CallInst *MallocCall, Function *F, bool IsStart,
                    SmallVectorImpl<Instruction *> &InstsToRemove) {
  Value *ContFrame = nullptr;
  if (MallocCall) {
    if (IsStart) {
      ContFrame = MallocCall;
      InstsToRemove.push_back(MallocCall);

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
  return ContFrame;
}

void LegacyCleanupContinuationsPassImpl::processContinuation(
    Function *StartFunc, ContinuationData &FuncData) {
  auto *Void = Type::getVoidTy(Context);
  LLVM_DEBUG(dbgs() << "Processing function: " << StartFunc->getName() << "\n");
  bool IsEntry = StartFunc->hasMetadata(DXILContHelper::MDEntryName);
  // The start function must come first to setup FuncData.NewStart and
  // ContMDTuple which is used by processing the resume functions.
  assert(StartFunc == FuncData.Functions[0]);
  MDTuple *ContMDTuple = nullptr;

  SmallVector<Function *> ToRemove;
  struct NewFunctionInfo {
    Function *Func;
    bool IsStart;
  };
  SmallVector<NewFunctionInfo> NewFuncs;

  for (auto *F : FuncData.Functions) {
    if (F != StartFunc) {
      // Entry marker should only be on the start and not on resume functions
      F->eraseMetadata(Context.getMDKindID(DXILContHelper::MDEntryName));
      // Same for stacksize
      F->eraseMetadata(Context.getMDKindID(DXILContHelper::MDStackSizeName));
      // Set same linkage as for start function
      F->setLinkage(StartFunc->getLinkage());
    }

    // Ignore the stub created for the coroutine passes
    if (F->empty())
      return;

    LLVM_DEBUG(dbgs() << "Processing function part: " << F->getName() << "\n");

    bool IsStart = F == StartFunc; // If this is the continuation start
    SmallVector<Type *> AllArgTypes;
    SmallVector<Value *> AllArgValues;
    SmallVector<Instruction *> InstsToRemove;
    AttributeList FAttrs = F->getAttributes();
    SmallVector<AttributeSet> ParamAttrs;

    // Use all arguments except the last (pre-allocated buffer for the
    // coroutine passes) for the continuation start
    if (IsStart) {
      unsigned ArgNo = 0;
      assert(F->arg_size() >= 1 && "Entry function has at least one argument");
      for (auto Arg = F->arg_begin(), ArgEnd = F->arg_end() - 1; Arg != ArgEnd;
           Arg++) {
        AllArgTypes.push_back(Arg->getType());
        AllArgValues.push_back(Arg);
        ParamAttrs.push_back(FAttrs.getParamAttrs(ArgNo));
        ArgNo++;
      }
    } else {
      B.SetInsertPoint(&*F->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
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
      forEachCall(*ContFree,
                  [&](CallInst &CI) { InstsToRemove.push_back(&CI); });
    }

    // Find the continuation state pointer, either returned by the malloc or
    // given as an argument
    Value *ContFrame =
        getContFrame(FuncData.MallocCall, F, IsStart, InstsToRemove);

    // Create new empty function
    F->eraseMetadata(FuncData.MD->getMetadataID());
    auto *NewFuncTy = FunctionType::get(Void, AllArgTypes, false);
    Function *NewFunc =
        CompilerUtils::cloneFunctionHeader(*F, NewFuncTy, ParamAttrs);
    NewFunc->takeName(F);
    NewFuncs.push_back({NewFunc, IsStart});

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
    B.SetInsertPoint(&*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    if (IsStart) {
      FuncData.NewStart = NewFunc;
      ContMDTuple =
          MDTuple::get(Context, {ValueAsMetadata::get(FuncData.NewStart)});
    }
    handleFunctionEntry(FuncData, NewFunc, IsEntry);

    // Handle the function body
    // Use the global continuation state
    ContFrame->replaceAllUsesWith(
        B.CreateBitOrPointerCast(FuncData.NewContState, ContFrame->getType()));

    // Handle the function returns
    for (auto &BB : make_early_inc_range(*NewFunc)) {
      auto *I = BB.getTerminator();
      if (I->getOpcode() == Instruction::Ret) {
        handleContinue(FuncData, I);
      } else if (I->getOpcode() == Instruction::Unreachable) {
        if (auto *Call = dyn_cast<CallInst>(--I->getIterator())) {
          if (auto *Called = Call->getCalledFunction()) {
            if (Called->getName() == "continuation.return")
              handleReturn(FuncData, Call);
          }
        }
      }
    }

    for (auto *I : InstsToRemove)
      I->eraseFromParent();

    // Remove the old function
    F->replaceAllUsesWith(ConstantExpr::getBitCast(NewFunc, F->getType()));
    ToRemove.push_back(F);

    // Update metadata
    assert(ContMDTuple != nullptr);
    NewFunc->setMetadata(DXILContHelper::MDContinuationName, ContMDTuple);
  }

  // Register count analysis needs to wait until all functions have been
  // processed above, turning rets into continuation.[wait]continue calls.
  for (auto [NewFunc, IsStart] : NewFuncs) {
    if (!IsStart) {
      uint32_t IncomingRegisterCount = getIncomingRegisterCount(NewFunc);
      DXILContHelper::setIncomingRegisterCount(NewFunc, IncomingRegisterCount);
    }
  }

  for (auto *F : ToRemove)
    F->eraseFromParent();
}

void LegacyCleanupContinuationsPassImpl::handleFunctionEntry(
    ContinuationData &Data, Function *F, bool IsEntry) {
  bool IsStart = F == Data.NewStart;

  // Create alloca to keep the continuation state
  uint64_t ContStateNumI32s = divideCeil(Data.ContStateBytes, RegisterBytes);
  uint64_t NeededStackSize = Data.getContStateStackBytes();
  auto *ContStateTy = ArrayType::get(I32, ContStateNumI32s);
  Data.NewContState = B.CreateAlloca(ContStateTy, nullptr, "cont.state");

  if (IsStart) {
    // Add function metadata that stores how big the continuation state is in
    // bytes
    DXILContHelper::setContinuationStateByteCount(*F, Data.ContStateBytes);
    if (NeededStackSize) {
      // Add to continuation stack size metadata
      DXILContHelper::addStackSize(F, NeededStackSize);
    }
  } else if (NeededStackSize) {
    // Obtain current CSP
    auto *CspOffsetPtr = B.CreateCall(getContinuationStackOffset(M));
    auto *CspType = getContinuationStackOffsetType(M.getContext());
    auto *Offset = B.CreateLoad(CspType, CspOffsetPtr);
    auto *Ptr = continuationStackOffsetToPtr(
        B, Offset, *(GpurtLibrary ? GpurtLibrary : &M), CrossInliner);

    // Obtain ptr to continuation state on stack,
    // and copy continuation state from global into local variable
    Value *ContStateOnStack =
        B.CreateGEP(B.getInt8Ty(), Ptr, B.getInt64(-NeededStackSize));
    createCopy(
        B, Data.NewContState,
        B.CreateBitOrPointerCast(ContStateOnStack,
                                 ContStateTy->getPointerTo(
                                     Ptr->getType()->getPointerAddressSpace())),
        ContStateTy);

    // Deallocate continuation stack space.
    // The generated IR is partially redundant with the above,
    // as the new CSP is just ContStateOnStack from above.
    // However, we need to do the copy first and only then deallocate.
    moveContinuationStackOffset(B, -NeededStackSize);
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
void LegacyCleanupContinuationsPassImpl::handleContinue(ContinuationData &Data,
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
    handleSingleContinue(Data, Call, ResumeFun);
  }

  if (BB->empty()) {
    assert(BB->hasNPredecessorsOrMore(0) &&
           "Handled all continues but the block still has predecessors left");
    BB->eraseFromParent();
  }
}

void LegacyCleanupContinuationsPassImpl::handleSingleContinue(
    ContinuationData &Data, CallInst *Call, Value *ResumeFun) {
  // Pass resume address as argument
  B.SetInsertPoint(Call);
  auto *ReturnAddrInt = B.CreatePtrToInt(ResumeFun, I64);

  auto *CpsType = getContinuationStackOffsetType(Call->getContext());
  auto *CspFun = getContinuationStackOffset(*Call->getModule());

  // Write local continuation state to stack and registers
  uint64_t NeededStackSize = Data.getContStateStackBytes();
  if (NeededStackSize) {
    // Allocate continuation stack space
    Value *ContStateOnStackOffset =
        moveContinuationStackOffset(B, NeededStackSize).first;
    auto *ContStateOnStackPtr = continuationStackOffsetToPtr(
        B, ContStateOnStackOffset, *(GpurtLibrary ? GpurtLibrary : &M),
        CrossInliner);
    // Copy continuation state from local variable into global
    auto *ContStateTy = Data.NewContState->getAllocatedType();
    createCopy(
        B,
        B.CreateBitOrPointerCast(
            ContStateOnStackPtr,
            ContStateTy->getPointerTo(
                ContStateOnStackPtr->getType()->getPointerAddressSpace())),
        Data.NewContState, ContStateTy);
  }

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
void LegacyCleanupContinuationsPassImpl::handleReturn(ContinuationData &Data,
                                                      CallInst *ContRet) {
  LLVM_DEBUG(dbgs() << "Converting return to continue: " << *ContRet << "\n");
  bool IsEntry = isa<UndefValue>(ContRet->getArgOperand(0));
  B.SetInsertPoint(ContRet);
  if (IsEntry) {
    assert(ContRet->arg_size() == 1 &&
           "Entry functions ignore the return value");
    B.CreateCall(Complete);
  } else {
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

LegacyCleanupContinuationsPassImpl::LegacyCleanupContinuationsPassImpl(
    llvm::Module &Mod, llvm::Module *GpurtLibrary,
    llvm::ModuleAnalysisManager &AnalysisManager)
    : M{Mod}, Context{M.getContext()}, B{Context}, GpurtLibrary{GpurtLibrary} {
  AnalysisManager.getResult<DialectContextAnalysis>(M);
  ContMalloc = M.getFunction("continuation.malloc");
  ContFree = M.getFunction("continuation.free");
}

llvm::PreservedAnalyses LegacyCleanupContinuationsPassImpl::run() {
  bool Changed = false;

  // Map the entry function of a continuation to the analysis result
  for (auto &F : M.functions()) {
    if (F.empty())
      continue;

    if (auto *MD = F.getMetadata(DXILContHelper::MDContinuationName)) {
      analyzeContinuation(F, MD);
    } else if (auto Stage = lgc::rt::getLgcRtShaderStage(&F);
               Stage && *Stage == lgc::rt::RayTracingShaderStage::Traversal) {
      Changed = true;
      // Add !continuation metadata to Traversal after coroutine passes.
      // The traversal loop is written as like the coroutine passes were applied
      // manually.
      MDTuple *ContMDTuple = MDTuple::get(Context, {ValueAsMetadata::get(&F)});
      F.setMetadata(DXILContHelper::MDContinuationName, ContMDTuple);
    }
  }

  // Check if the continuation state is used in any function part
  for (auto &FuncData : ToProcess) {
    finalizeContinuationData(*FuncData.first, FuncData.second);
    MaxContStateBytes =
        std::max(MaxContStateBytes, FuncData.second.ContStateBytes);
  }

  Changed |= !ToProcess.empty();

  if (!ToProcess.empty()) {
    I32 = Type::getInt32Ty(Context);
    I64 = Type::getInt64Ty(Context);
    Continue = getContinuationContinue(M);
    WaitContinue = getContinuationWaitContinue(M);
    Complete = getContinuationComplete(M);

    for (auto &FuncData : ToProcess) {
      processContinuation(FuncData.first, FuncData.second);
    }

    fixupDxilMetadata(M);
  }

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

} // namespace

llvm::PreservedAnalyses LegacyCleanupContinuationsPass::run(
    llvm::Module &Mod, llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the cleanup-continuations pass\n");
  LegacyCleanupContinuationsPassImpl Impl(Mod, GpurtLibrary, AnalysisManager);
  return Impl.run();
}
