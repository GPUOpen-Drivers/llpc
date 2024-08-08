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

//= LegacyCleanupContinuations.cpp - Post-process output of coroutine passes =//
//
// Convert the result from the coroutine passes to something more suitable for
// the compiler backend.
//
// Instead of return values, use continue and waitContinue intrinsics.
// Add arguments to resume functions, which are the return values of the called
// continuation.
//
//===----------------------------------------------------------------------===//

#include "compilerutils/CompilerUtils.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/MathExtras.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "legacy-cleanup-continuations"

namespace {

class LegacyCleanupContinuationsPassImpl {
public:
  LegacyCleanupContinuationsPassImpl(llvm::Module &Mod, llvm::ModuleAnalysisManager &AnalysisManager);

  PreservedAnalyses run();

private:
  struct ContinuationData {
    /// All functions belonging to this continuation, the entry function is the
    /// first one
    SmallVector<Function *> Functions;
    /// Size of the continuation state in byte
    uint32_t ContStateBytes = 0;
    CallInst *MallocCall = nullptr;
    MDNode *MD = nullptr;
    // The continuation state on the CPS stack
    Value *NewContState = nullptr;
    SmallVector<CallInst *> NewReturnContinues;
    /// Cleaned entry function, used to replace metadata
    Function *NewStart = nullptr;

    // Returns the number of bytes used on the CPS stack for the continuation
    // state.
    uint32_t getContStateStackBytes() const { return alignTo(ContStateBytes, RegisterBytes); }
  };

  void analyzeContinuation(Function &F, MDNode *MD);
  // Run analysis parts that need to wait until all resume functions have been
  // collected
  void finalizeContinuationData(Function &StartFunc, ContinuationData &Data);
  void processContinuation(Function *StartFunc, ContinuationData &FuncData);
  void handleFunctionEntry(ContinuationData &Data, Function *F, bool IsEntry);
  void handleContinue(ContinuationData &Data, Instruction *Ret);
  void handleSingleContinue(ContinuationData &Data, CallInst *Call, Value *ResumeFun);
  void handleReturn(ContinuationData &Data, lgc::ilcps::ReturnOp &ContRet);

  Module &M;
  LLVMContext &Context;
  llvm::FunctionAnalysisManager &FAM;
  llvm_dialects::Builder B;
  Type *I32 = nullptr;
  Type *I64 = nullptr;
  Function *ContMalloc = nullptr;
  Function *ContFree = nullptr;
  MapVector<Function *, ContinuationData> ToProcess;
  CompilerUtils::CrossModuleInliner CrossInliner;
};

/// Find the original call that created the continuation token and the matching
/// resume function for a return value.
///
/// Returns a map (origin BB, (call that created the continuation token, resume
/// function)).
DenseMap<BasicBlock *, std::pair<CallInst *, Value *>> findTokenOrigin(BasicBlock *BB, Value *V,
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

  auto RegisterTokenOrigin = [&Result](BasicBlock *TheBB, Value *Token, Value *TheResumeFun) {
    assert(isa<Constant>(TheResumeFun) && "Resume function should be a constant function");
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

    for (auto CallEntry : llvm::zip(CallPhi->blocks(), CallPhi->incoming_values())) {
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

void LegacyCleanupContinuationsPassImpl::analyzeContinuation(Function &F, MDNode *MD) {
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
    Data.ContStateBytes = cast<ConstantInt>(Data.MallocCall->getArgOperand(0))->getSExtValue();
  }
}

void LegacyCleanupContinuationsPassImpl::finalizeContinuationData(Function &StartFunc, ContinuationData &FuncData) {
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
    if (isa<Constant>(U) || isa<lgc::cps::AsContinuationReferenceOp>(U)) {
      Worklist.append(U->user_begin(), U->user_end());
      continue;
    }
    assert(isa<CallInst>(U) && "User of a resume function should be a call to continue");
    auto *Inst = cast<CallInst>(U);
    if (auto Count = ContHelper::ReturnedRegisterCount::tryGetValue(Inst)) {
      assert((!RegCount || *RegCount == *Count) && "Got different returned registercounts in continues to "
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

Value *getContFrame(CallInst *MallocCall, Function *F, bool IsStart, SmallVectorImpl<Instruction *> &InstsToRemove) {
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

void LegacyCleanupContinuationsPassImpl::processContinuation(Function *StartFunc, ContinuationData &FuncData) {
  auto *Void = Type::getVoidTy(Context);
  LLVM_DEBUG(dbgs() << "Processing function: " << StartFunc->getName() << "\n");
  bool IsEntry = StartFunc->hasMetadata(ContHelper::MDEntryName);
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
      F->eraseMetadata(Context.getMDKindID(ContHelper::MDEntryName));
      // Same for stacksize
      F->eraseMetadata(Context.getMDKindID(ContHelper::MDStackSizeName));
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
      for (auto Arg = F->arg_begin(), ArgEnd = F->arg_end() - 1; Arg != ArgEnd; Arg++) {
        AllArgTypes.push_back(Arg->getType());
        AllArgValues.push_back(Arg);
        ParamAttrs.push_back(FAttrs.getParamAttrs(ArgNo));
        ArgNo++;
      }
    } else {
      B.SetInsertPoint(&*F->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());

      AllArgTypes.push_back(B.getInt64Ty()); // Dummy return address for resume functions
      AllArgValues.push_back(nullptr);

      // Find arguments from lgc.ilcps.getreturnvalue calls
      for (auto &I : F->getEntryBlock()) {
        if (auto *Intr = dyn_cast<lgc::ilcps::GetReturnValueOp>(&I)) {
          AllArgTypes.push_back(Intr->getType());
          AllArgValues.push_back(Intr);
          InstsToRemove.push_back(Intr);
        }
      }
    }

    // Find the free call if there is one
    if (ContFree) {
      forEachCall(*ContFree, [&](CallInst &CI) { InstsToRemove.push_back(&CI); });
    }

    // Find the continuation state pointer, either returned by the malloc or
    // given as an argument
    Value *ContFrame = getContFrame(FuncData.MallocCall, F, IsStart, InstsToRemove);

    // Try to eliminate unnecessary continuation state accesses
    // of values that are still available as SSA values by a simple
    // store-to-load forwarding routine.
    // Ideally, LLVM coro passes should do better and not emit these
    // loads to begin with.
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(*F);
    forwardContinuationFrameStoreToLoad(DT, ContFrame);

    // Create new empty function
    F->eraseMetadata(FuncData.MD->getMetadataID());
    auto *NewFuncTy = FunctionType::get(Void, AllArgTypes, false);
    Function *NewFunc = CompilerUtils::cloneFunctionHeader(*F, NewFuncTy, ParamAttrs);
    NewFunc->takeName(F);
    NewFuncs.push_back({NewFunc, IsStart});

    // Transfer code from old function to new function
    llvm::moveFunctionBody(*F, *NewFunc);

    // Set arg names for new function
    // Skip the dummy return address for non-start functions
    for (unsigned Idx = 0; Idx != NewFunc->getFunctionType()->params().size(); ++Idx) {
      Value *OldVal = AllArgValues[Idx];
      // Skip the dummy return address.
      if (!OldVal)
        continue;

      Argument *Arg = NewFunc->getArg(Idx);
      Arg->setName(OldVal->getName());
      OldVal->replaceAllUsesWith(Arg);

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
      ContMDTuple = MDTuple::get(Context, {ValueAsMetadata::get(FuncData.NewStart)});
    }
    handleFunctionEntry(FuncData, NewFunc, IsEntry);

    // Handle the function body

    if (FuncData.NewContState) {
      // Bitcast new cont state to the pointer type used by coro passes, but
      // preserve the address space. Uses of the pointer are then fixed to also
      // use the correct address space.
      PointerType *UsedContFrameTy = cast<PointerType>(ContFrame->getType());
      Value *CastNewContState = B.CreateBitCast(
          FuncData.NewContState,
          getWithSamePointeeType(UsedContFrameTy, FuncData.NewContState->getType()->getPointerAddressSpace()));
      CompilerUtils::replaceAllPointerUses(&B, ContFrame, CastNewContState, InstsToRemove);
    } else {
      // If there is no continuation state, replace it with a poison
      // value instead of a zero-sized stack allocation.
      // This leads to nicer tests.
      ContFrame->replaceAllUsesWith(PoisonValue::get(ContFrame->getType()));
    }

    // Handle the function returns
    for (auto &BB : make_early_inc_range(*NewFunc)) {
      auto *I = BB.getTerminator();
      if (I->getOpcode() == Instruction::Ret) {
        handleContinue(FuncData, I);
      } else if (I->getOpcode() == Instruction::Unreachable && BB.size() > 1) {
        if (auto *Call = dyn_cast<CallInst>(--I->getIterator())) {
          if (auto *ContRet = dyn_cast<lgc::ilcps::ReturnOp>(Call))
            handleReturn(FuncData, *ContRet);
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
    NewFunc->setMetadata(ContHelper::MDContinuationName, ContMDTuple);
  }

  // Register count analysis needs to wait until all functions have been
  // processed above, turning rets into continuation.[wait]continue calls.
  for (auto [NewFunc, IsStart] : NewFuncs) {
    if (!IsStart) {
      uint32_t IncomingRegisterCount = getIncomingRegisterCount(NewFunc);
      ContHelper::IncomingRegisterCount::setValue(NewFunc, IncomingRegisterCount);
    }
  }

  for (auto *F : ToRemove)
    F->eraseFromParent();
}

void LegacyCleanupContinuationsPassImpl::handleFunctionEntry(ContinuationData &Data, Function *F, bool IsEntry) {
  uint64_t NeededStackSize = Data.getContStateStackBytes();
  bool IsStart = F == Data.NewStart;

  if (IsStart) {
    // Add function metadata that stores how big the continuation state is in
    // bytes
    // Technically, continuation state includes the spilled payload here.
    // However, we want to exclude it here for statistics.
    uint32_t PayloadSpillSize = ContHelper::StackSize::tryGetValue(F).value_or(0);
    assert(Data.ContStateBytes >= PayloadSpillSize);
    ContHelper::ContinuationStateByteCount::setValue(F, Data.ContStateBytes - PayloadSpillSize);
  }

  if (NeededStackSize) {
    Value *ContStateOnStack = nullptr;
    if (IsStart) {
      ContHelper::StackSize::setValue(F, NeededStackSize);

      ContStateOnStack = B.create<lgc::cps::AllocOp>(B.getInt32(NeededStackSize));
    } else {
      ContStateOnStack = B.create<lgc::cps::PeekOp>(B.getInt32(NeededStackSize));
    }

    ContStateOnStack->setName("cont.state.stack.segment");

    uint64_t ContStateNumI32s = divideCeil(Data.ContStateBytes, RegisterBytes);
    auto *ContStateTy = ArrayType::get(I32, ContStateNumI32s);

    // Peek into CSP stack to obtain continuation state.
    // This can be handled in the same way for start and resume functions,
    // because for start functions we already allocated space above.
    Data.NewContState =
        B.CreateBitCast(ContStateOnStack, ContStateTy->getPointerTo(lgc::cps::stackAddrSpace), "cont.state");
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
///   call void @lgc.ilcps.continue(i64 %foo, i64
///     %resume_addr, <foo args>) !continuation.registercount !0
///   unreachable
///
/// Also handles cases where the token and resume function are behind a phi.
void LegacyCleanupContinuationsPassImpl::handleContinue(ContinuationData &Data, Instruction *Ret) {
  // Find the function call that generates the token
  LLVM_DEBUG(dbgs() << "Converting ret to continue: " << *Ret << "\nArgument: " << *Ret->getOperand(0) << "\n");
  auto *BB = Ret->getParent();
  SmallVector<Instruction *> ToRemove;
  ToRemove.push_back(Ret);
  auto Calls = findTokenOrigin(Ret->getParent(), Ret->getOperand(0), ToRemove);

  for (auto *I : ToRemove)
    I->eraseFromParent();

  for (auto &Entry : Calls) {
    LLVM_DEBUG(dbgs() << "Handling call: " << *Entry.second.first << " with resume function " << Entry.second.second
                      << "\n");
    auto *Call = Entry.second.first;
    auto *ResumeFun = Entry.second.second;
    handleSingleContinue(Data, Call, ResumeFun);
  }

  if (BB->empty()) {
    assert(BB->hasNPredecessorsOrMore(0) && "Handled all continues but the block still has predecessors left");
    BB->eraseFromParent();
  }
}

void LegacyCleanupContinuationsPassImpl::handleSingleContinue(ContinuationData &Data, CallInst *Call,
                                                              Value *ResumeFun) {
  // Pass resume address as argument
  B.SetInsertPoint(Call);

  auto *ContinuationReference = B.create<lgc::cps::AsContinuationReferenceOp>(I64, ResumeFun);

  bool IsWait = ContHelper::isWaitAwaitCall(*Call);

  // The jump call tail argument list needs to start with the return address.
  Value *JumpAddr = B.CreatePointerCast(Call->getCalledOperand(), I64);
  SmallVector<Value *> TailArgs{Call->arg_begin() + (IsWait ? 1 : 0), Call->arg_end()};
  TailArgs.insert(TailArgs.begin(), ContinuationReference);

  CallInst *Jump =
      B.create<lgc::cps::JumpOp>(JumpAddr, -1, PoisonValue::get(StructType::get(B.getContext())), TailArgs);

  Jump->copyMetadata(*Call);
  ContHelper::removeIsWaitAwaitMetadata(*Jump);

  if (IsWait)
    ContHelper::setWaitMask(*Jump, cast<ConstantInt>(Call->getArgOperand(0))->getSExtValue());
  assert(ContHelper::OutgoingRegisterCount::tryGetValue(Jump) && "Missing registercount metadata!");

  // Remove instructions at the end of the block
  auto *Unreachable = B.CreateUnreachable();
  for (auto &I : make_early_inc_range(reverse(*Jump->getParent()))) {
    if (&I == Unreachable)
      break;
    I.eraseFromParent();
  }
}

/// Transform
///   call void (i64, ...) @lgc.ilcps.return(i64 %returnaddr, <return
///   value>) unreachable
/// to
///   <decrement CSP>
///   call void @lgc.ilcps.continue(i64 %returnaddr, <return value>)
///   unreachable
void LegacyCleanupContinuationsPassImpl::handleReturn(ContinuationData &Data, lgc::ilcps::ReturnOp &ContRet) {
  LLVM_DEBUG(dbgs() << "Converting return to continue: " << ContRet << "\n");
  bool IsEntry = isa<UndefValue>(ContRet.getReturnAddr());
  B.SetInsertPoint(&ContRet);

  uint32_t NeededStackSize = Data.getContStateStackBytes();
  if (NeededStackSize > 0)
    B.create<lgc::cps::FreeOp>(B.getInt32(NeededStackSize));

  if (IsEntry) {
    assert(ContRet.getArgs().empty() && "Entry functions ignore the return value");

    llvm::terminateShader(B, &ContRet);
  } else {
    // Create the call to lgc.ilcps.continue, but with the same argument list
    // as for lgc.ilcps.return. The CSP is being set during
    // DXILContPostProcess.
    // Append the dummy return address as well.
    SmallVector<Value *, 2> RetTail{ContRet.getArgs()};
    auto *ContinueOp = B.create<lgc::ilcps::ContinueOp>(ContRet.getReturnAddr(), PoisonValue::get(B.getInt32Ty()),
                                                        PoisonValue::get(B.getInt64Ty()), RetTail);
    Data.NewReturnContinues.push_back(ContinueOp);

    ContinueOp->copyMetadata(ContRet);
    assert(ContHelper::OutgoingRegisterCount::tryGetValue(ContinueOp) && "Missing registercount metadata!");
    ContRet.eraseFromParent();
  }
}

LegacyCleanupContinuationsPassImpl::LegacyCleanupContinuationsPassImpl(llvm::Module &Mod,
                                                                       llvm::ModuleAnalysisManager &AnalysisManager)
    : M{Mod}, Context{M.getContext()},
      FAM{AnalysisManager.getResult<FunctionAnalysisManagerModuleProxy>(Mod).getManager()}, B{Context} {
  AnalysisManager.getResult<DialectContextAnalysis>(M);
  ContMalloc = M.getFunction("continuation.malloc");
  ContFree = M.getFunction("continuation.free");
}

PreservedAnalyses LegacyCleanupContinuationsPassImpl::run() {
  bool Changed = false;

  // Map the entry function of a continuation to the analysis result
  for (auto &F : M.functions()) {
    if (F.empty())
      continue;
    if (auto *MD = F.getMetadata(ContHelper::MDContinuationName)) {
      analyzeContinuation(F, MD);
    } else {
      auto ShaderStage = lgc::rt::getLgcRtShaderStage(&F);
      if (ShaderStage == lgc::rt::RayTracingShaderStage::Traversal ||
          ShaderStage == lgc::rt::RayTracingShaderStage::KernelEntry) {
        Changed = true;
        // Add !continuation metadata to KernelEntry and Traversal after
        // coroutine passes. The traversal loop is written as like the coroutine
        // passes were applied manually.
        MDTuple *ContMDTuple = MDTuple::get(Context, {ValueAsMetadata::get(&F)});
        F.setMetadata(ContHelper::MDContinuationName, ContMDTuple);
      }
    }
  }

  // Check if the continuation state is used in any function part
  for (auto &FuncData : ToProcess) {
    finalizeContinuationData(*FuncData.first, FuncData.second);
  }

  Changed |= !ToProcess.empty();

  if (!ToProcess.empty()) {
    I32 = Type::getInt32Ty(Context);
    I64 = Type::getInt64Ty(Context);

    for (auto &FuncData : ToProcess) {
      processContinuation(FuncData.first, FuncData.second);
    }

    fixupDxilMetadata(M);
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace

llvm::PreservedAnalyses LegacyCleanupContinuationsPass::run(llvm::Module &Mod,
                                                            llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the cleanup-continuations pass\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Mod);
  LegacyCleanupContinuationsPassImpl Impl(Mod, AnalysisManager);
  return Impl.run();
}
