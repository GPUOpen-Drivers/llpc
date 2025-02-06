//===- CleanupContinuations.cpp - Post-process output of coroutine passes -===//
//
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//===----------------------------------------------------------------------===//
//
// Convert the result from the coroutine passes to something more suitable for
// the compiler backend.
// 1. Replace returning handle with lgc.cps.jump() with the right continuation
//    reference.
// 2. Replace @lgc.cps.complete with simple `ret`, which means thread
//    termination.
// 3. Edit function signatures, like removing coroutine frame pointer argument,
//    adding needed ones (state, rcr, returned_values) for resume function.
// 4. Allocating/freeing cps stack space as needed.
//===----------------------------------------------------------------------===//

#include "compilerutils/CompilerUtils.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/CpsStackLowering.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

using namespace llvm;
using namespace lgc;

#define DEBUG_TYPE "cleanup-continuations"

namespace {

class CleanupContinuationsPassImpl {
public:
  CleanupContinuationsPassImpl(llvm::Module &M, llvm::Module &GpurtLibrary, llvm::ModuleAnalysisManager &AM);

  PreservedAnalyses run();

private:
  struct ContinuationData {
    /// All functions belonging to this continuation, the entry function is the
    /// first one
    SmallVector<Function *> Functions;
    SmallVector<Function *> NewFunctions;
    SmallVector<CallInst *> CpsIntrinsicCalls;
    /// Size of the continuation state in byte
    bool IsStart = true;
    uint32_t ContStateBytes = 0;
    CallInst *MallocCall = nullptr;
    MDNode *MD = nullptr;
  };

  void removeContFreeCall(Function *F, Function *ContFree);
  Value *getContinuationFramePtr(Function *F, bool IsStart, const ContinuationData &ContinuationInfo,
                                 SmallVector<Instruction *> *InstsToRemove = nullptr);
  void freeCpsStack(Function *F, ContinuationData &CpsInfo);
  void updateCpsStack(Function *F, Function *NewFunc, bool IsStart, ContinuationData &CpsInfo);
  void analyzeContinuation(Function &F, MDNode *MD);
  void processContinuations();
  void handleContinue(ContinuationData &Data, Instruction *Ret);
  void handleSingleContinue(ContinuationData &Data, CallInst *Call, Value *ResumeFun);
  void lowerIntrinsicCall(Function *F, ContinuationData &Data);
  bool handleIntrinsics(llvm::ModuleAnalysisManager &AnalysisManager);
  void handleContStackIntrinsic(FunctionAnalysisManager &FAM, Function &F);
  void lowerGetResumePoint(Module &Mod);
  bool lowerCompleteOp(Module &Mod);

  llvm::Module &Mod;
  llvm::ModuleAnalysisManager &AnalysisManager;
  llvm_dialects::Builder Builder;
  Function *ContMalloc = nullptr;
  Function *ContFree = nullptr;
  MapVector<Function *, ContinuationData> ToProcess;
  llvm::Module *GpurtLibrary = nullptr;
  std::optional<CpsStackLowering> StackLowering;
  Function *GetGlobalMemBase = nullptr;
};

/// Find the original call that created the continuation token and the matching
/// resume function for a return value.
///
/// Returns a map (origin BB, (call that created the continuation token, resume
/// function)).
static DenseMap<BasicBlock *, std::pair<CallInst *, Value *>>
findTokenOrigin(BasicBlock *BB, Value *V, SmallVectorImpl<Instruction *> &ToRemove) {
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
      assert(isa<Constant>(ResumeFunEntry) && "Resume function should be a constant function");

      Value *CInst = std::get<1>(CallEntry);

      // Strip away bitcasts -- this can happen with multiple token types
      if (auto *TokenBitcast = dyn_cast<BitCastOperator>(CInst))
        CInst = TokenBitcast->getOperand(0);

      assert(isa<CallInst>(CInst) && "Phi should come from a call");

      Result.insert(std::make_pair(PhiBB, std::make_pair(cast<CallInst>(CInst), ResumeFunEntry)));
    }
  } else {
    assert(isa<Constant>(ResumeFun) && "Resume function should be a constant function");
    // Strip away bitcasts -- this can happen with multiple token types
    if (auto *TokenBitcast = dyn_cast<BitCastOperator>(Call))
      Call = TokenBitcast->getOperand(0);
    assert(isa<CallInst>(Call) && "Call should be a CallInst");
    auto *CallI = cast<CallInst>(Call);
    Result.insert(std::make_pair(BB, std::make_pair(CallI, ResumeFun)));
  }
  return Result;
}

void CleanupContinuationsPassImpl::analyzeContinuation(Function &F, MDNode *MD) {
  Function *EntryF = &F;

  // Only analyze main continuation
  if (MD) {
    auto *MDTup = cast<MDTuple>(MD);
    EntryF = mdconst::extract<Function>(MDTup->getOperand(0));
  }

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
    Data.ContStateBytes = cast<ConstantInt>(Data.MallocCall->getArgOperand(0))->getSExtValue();
  }
}

void CleanupContinuationsPassImpl::updateCpsStack(Function *F, Function *NewFunc, bool IsStart,
                                                  ContinuationData &CpsInfo) {

  Builder.SetInsertPointPastAllocas(NewFunc);
  Value *CpsStack = nullptr;
  if (IsStart) {
    CpsStack = Builder.create<cps::AllocOp>(Builder.getInt32(CpsInfo.ContStateBytes));
    CpsStack->setName("cont.state.stack.segment");
    ContHelper::StackSize::setValue(NewFunc, CpsInfo.ContStateBytes);
  } else {
    CpsStack = Builder.create<cps::PeekOp>(Builder.getInt32(CpsInfo.ContStateBytes));
  }

  SmallVector<Instruction *> ToBeRemoved;
  Value *ContFrame = getContinuationFramePtr(F, IsStart, CpsInfo, &ToBeRemoved);

  if (CpsInfo.ContStateBytes != 0) {
    CompilerUtils::replaceAllPointerUses(ContFrame, CpsStack, ToBeRemoved);
  } else {
    // If there is no continuation state, replace it with a poison
    // value instead of a zero-sized stack allocation.
    // This leads to nicer tests.
    ContFrame->replaceAllUsesWith(PoisonValue::get(ContFrame->getType()));
  }

  for (auto *I : reverse(ToBeRemoved))
    I->eraseFromParent();
}

static void updateFunctionArgs(Function *OldFunc, Function *NewFunc, const SmallVector<Value *> &AllArgValues) {
  // Set arg names for new function
  for (auto [OldVal, NewArg] : llvm::zip_equal(AllArgValues, NewFunc->args())) {
    if (OldVal) {
      NewArg.setName(OldVal->getName());
      OldVal->replaceAllUsesWith(&NewArg);
    }
  }
}

static void buildArgInfos(Function *F, bool IsStart, SmallVector<Type *> &AllArgTypes,
                          SmallVector<Value *> &AllArgValues, uint32_t &StartReturnArg,
                          SmallVector<AttributeSet> &ParamAttrs, SmallVector<Instruction *> &InstsToRemove,
                          SmallVector<lgc::ilcps::GetReturnValueOp *> &ReturnValueOps) {
  AttributeList FAttrs = F->getAttributes();
  if (IsStart) {
    unsigned ArgNo = 0;
    assert(F->arg_size() >= 1 && "Entry function has at least one argument");
    // Use all arguments except the last (pre-allocated buffer for the
    // coroutine passes) for the continuation start
    for (auto Arg = F->arg_begin(), ArgEnd = F->arg_end() - 1; Arg != ArgEnd; Arg++) {
      AllArgTypes.push_back(Arg->getType());
      AllArgValues.push_back(Arg);
      ParamAttrs.push_back(FAttrs.getParamAttrs(ArgNo));
      ArgNo++;
    }
  } else {
    // Find arguments from lgc.ilcps.getreturnvalue calls
    for (auto &I : F->getEntryBlock()) {
      if (auto *Intr = dyn_cast<lgc::ilcps::GetReturnValueOp>(&I)) {
        StartReturnArg = AllArgValues.size();

        // The type is always a struct. Unfold it so we are able to use the same CpsArgIdx* indices as for non-resume
        // functions.
        auto *StructTy = cast<StructType>(Intr->getType());

        for (auto [Index, Ty] : llvm::enumerate(StructTy->elements())) {
          AllArgTypes.push_back(Ty);
          AllArgValues.push_back(nullptr);
        }

        InstsToRemove.push_back(Intr);
        ReturnValueOps.push_back(Intr);
      }
    }
  }
}

/// Find the continuation state pointer, either returned by the malloc or
/// given as an argument
Value *CleanupContinuationsPassImpl::getContinuationFramePtr(Function *F, bool IsStart,
                                                             const ContinuationData &ContinuationInfo,
                                                             SmallVector<Instruction *> *InstsToRemove) {
  if (!ContinuationInfo.MallocCall)
    return IsStart ? F->getArg(F->arg_size() - 1) : F->getArg(0);

  if (IsStart) {
    if (InstsToRemove)
      InstsToRemove->push_back(ContinuationInfo.MallocCall);
    return ContinuationInfo.MallocCall;
  }
  // Look for the load of the allocated pointer
  Instruction *Load = cast<Instruction>(F->getArg(0)->getUniqueUndroppableUser());
  if (InstsToRemove)
    InstsToRemove->push_back(Load); // Load needs to be eliminated
  return Load;
}

/// Remove call to continuation.free() in F, ContFree is the pointer to
/// declaration of continuation.free().
void CleanupContinuationsPassImpl::removeContFreeCall(Function *F, Function *ContFree) {
  for (auto *User : make_early_inc_range(ContFree->users())) {
    if (auto *Call = dyn_cast<CallInst>(User)) {
      if (Call->getFunction() == F) {
        Call->eraseFromParent();
        break;
      }
    }
  }
}

/// Insert cps.free() before the original function exits and lgc.cps.complete calls.
void CleanupContinuationsPassImpl::freeCpsStack(Function *F, ContinuationData &CpsInfo) {
  struct VisitState {
    ContinuationData &CpsInfo;
    llvm_dialects::Builder &Builder;
    Function *F;
  };
  VisitState State = {CpsInfo, Builder, F};
  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitState>()
          .addSet<cps::JumpOp, cps::CompleteOp>([](auto &State, auto &Instruction) {
            if (Instruction.getFunction() == State.F && State.CpsInfo.ContStateBytes) {
              State.Builder.SetInsertPoint(&Instruction);
              State.Builder.template create<cps::FreeOp>(State.Builder.getInt32(State.CpsInfo.ContStateBytes));
            }
          })
          .build();
  Visitor.visit(State, *F);
}

/// Handle lgc.cps.complete calls.
bool CleanupContinuationsPassImpl::lowerCompleteOp(Module &Mod) {
  struct VisitState {
    llvm_dialects::Builder &Builder;
    bool CompleteLowered;
  };

  VisitState State = {Builder, false};
  static auto Visitor = llvm_dialects::VisitorBuilder<VisitState>()
                            .add<cps::CompleteOp>([](VisitState &State, auto &Complete) {
                              State.Builder.SetInsertPoint(&Complete);
                              State.Builder.CreateRetVoid();
                              BasicBlock *BB = Complete.getParent();
                              BB->getTerminator()->eraseFromParent();
                              Complete.eraseFromParent();
                              State.CompleteLowered = true;
                            })
                            .build();

  Visitor.visit(State, Mod);
  return State.CompleteLowered;
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
      report_fatal_error("Found a jump call without "
                         "continuation returned registercount metadata");
    }
  }
  return RegCount.value();
}

void CleanupContinuationsPassImpl::processContinuations() {
  // Summarize of what to do here:
  // 1. Continuation Stack
  //    a.) cps.alloc() in start, and cps.peek() cps.free() in resume.
  //    b.) change the address space for cps stack to 32.
  // 2. prepare arguments passed to cps.jump and insert the call at the exit of
  //    start part.
  // 3. Edit resume signature to add the state and the return values. This adds the shader record index and the return
  //    address to the function signature.
  SmallVector<Function *> ToErase;
  for (auto &FuncData : ToProcess) {
    LLVM_DEBUG(dbgs() << "Processing function: " << FuncData.first->getName() << "\n");
    for (auto *F : FuncData.second.Functions) {
      // Not a new function but we want to run stack lowering on KernelEntry
      if (!F->hasMetadata(ContHelper::MDContinuationName)) {
        FuncData.second.NewFunctions.push_back(F);
        continue;
      }

      if (F != FuncData.first) {
        // Set same linkage as for start function
        F->setLinkage(FuncData.first->getLinkage());
        // Same for stacksize
        ContHelper::StackSize::reset(F);
      }

      // Ignore the stub created for the coroutine passes
      if (F->empty())
        continue;

      LLVM_DEBUG(dbgs() << "Processing function part: " << F->getName() << "\n");

      // If this is the continuation start
      bool IsStart = F == FuncData.first;

      SmallVector<Type *> AllArgTypes;
      SmallVector<Value *> AllArgValues;
      SmallVector<AttributeSet> ParamAttrs;
      SmallVector<Instruction *> InstsToRemove;
      SmallVector<lgc::ilcps::GetReturnValueOp *> ReturnValueOps;
      uint32_t StartReturnArg = 0;

      buildArgInfos(F, IsStart, AllArgTypes, AllArgValues, StartReturnArg, ParamAttrs, InstsToRemove, ReturnValueOps);

      if (ContFree)
        removeContFreeCall(F, ContFree);

      // Create new empty function
      if (FuncData.second.MD)
        F->eraseMetadata(FuncData.second.MD->getMetadataID());
      auto &Context = F->getContext();
      auto *NewFuncTy = FunctionType::get(Type::getVoidTy(Context), AllArgTypes, false);
      Function *NewFunc = CompilerUtils::cloneFunctionHeader(*F, NewFuncTy, ParamAttrs);
      NewFunc->takeName(F);

      // Create helper struct for return values and RAUW on them
      for (lgc::ilcps::GetReturnValueOp *RetValOp : ReturnValueOps) {
        Value *RetHelperStruct = PoisonValue::get(RetValOp->getType());
        Builder.SetInsertPointPastAllocas(RetValOp->getFunction());
        for (auto [Idx, RetArg] :
             llvm::enumerate(llvm::make_range(NewFunc->arg_begin() + StartReturnArg, NewFunc->arg_end())))
          RetHelperStruct = Builder.CreateInsertValue(RetHelperStruct, &RetArg, Idx);

        RetValOp->replaceAllUsesWith(RetHelperStruct);
      }

      ToErase.push_back(F);
      FuncData.second.NewFunctions.push_back(NewFunc);

      // Transfer code from old function to new function
      llvm::moveFunctionBody(*F, *NewFunc);

      auto &CpsInfo = FuncData.second;

      // Add function metadata that stores how big the continuation state is in bytes.
      // Technically, continuation state includes the spilled payload here.
      // However, we want to exclude it here for statistics.
      // TODO: Remove this once we can properly report payload size statistics in LowerRaytracingPipeline.
      if (IsStart) {
        const uint32_t PayloadSpillSize = ContHelper::StackSize::tryGetValue(NewFunc).value_or(0);
        assert(CpsInfo.ContStateBytes >= PayloadSpillSize);
        ContHelper::ContinuationStateByteCount::setValue(NewFunc, CpsInfo.ContStateBytes - PayloadSpillSize);
      }

      CpsInfo.IsStart = IsStart;

      if (CpsInfo.ContStateBytes)
        updateCpsStack(F, NewFunc, IsStart, CpsInfo);

      updateFunctionArgs(F, NewFunc, AllArgValues);

      freeCpsStack(NewFunc, CpsInfo);
      // Handle the function returns
      for (auto &BB : make_early_inc_range(*NewFunc)) {
        auto *I = BB.getTerminator();
        if (isa<ReturnInst>(I)) {
          handleContinue(FuncData.second, I);
        }
      }

      for (auto *I : InstsToRemove)
        I->eraseFromParent();

      // Replace the old function with the new one.
      F->replaceAllUsesWith(NewFunc);
      // Update the `ToProcess` for later processing.
      if (IsStart)
        FuncData.first = NewFunc;

      // Record lgc.rt intrinsic function calls.
      for (auto &IntrinsicFunc : Mod.functions()) {
        if (!lgc::rt::LgcRtDialect::isDialectOp(IntrinsicFunc))
          continue;

        llvm::forEachCall(IntrinsicFunc, [&](CallInst &CInst) {
          auto *Caller = CInst.getFunction();
          if (Caller != NewFunc)
            return;

          auto IntrImplEntry = llvm::findIntrImplEntryByIntrinsicCall(&CInst);
          if (IntrImplEntry == std::nullopt)
            return;

          CpsInfo.CpsIntrinsicCalls.push_back(&CInst);
        });
      }

      // Lower lgc.rt intrinsics
      lowerIntrinsicCall(NewFunc, CpsInfo);
    }

    for (Function *F : FuncData.second.NewFunctions) {
      if (FuncData.first != F) {
        uint32_t IncomingRegisterCount = getIncomingRegisterCount(F);
        ContHelper::IncomingRegisterCount::setValue(F, IncomingRegisterCount);
      }
    }
  }

  // Remove the old functions
  for (Function *F : ToErase)
    F->eraseFromParent();
}

/// Transform
///  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
///  %2 = inttoptr i32 %cr to ptr
///  %3 = call i32 %2(i32 %cr, i32 2, ...)
///  %4 = insertvalue { ptr, i32 } undef, ptr @test.resume.0, 0
///  %5 = insertvalue { ptr, i32 } %4, i32 %3, 1
///  ret { ptr, i32 } %5
///
///  To:
///  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
///  %cr2 = call i32 (...) @lgc.cps.as.continuation.reference(ptr
///                          @test.resume.0)
///   call void (...) @lgc.cps.jump(i32 %cr, i32 2,
///                                 i32 %cr2, ...)
///
/// Also handles cases where the token and resume function are behind a phi.
void CleanupContinuationsPassImpl::handleContinue(ContinuationData &Data, Instruction *Ret) {
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

void CleanupContinuationsPassImpl::handleSingleContinue(ContinuationData &Data, CallInst *Call, Value *ResumeFun) {
  Builder.SetInsertPoint(Call);

  SmallVector<Value *> TailArgs;
  Value *ResumeAddr = nullptr;
  Value *CR = nullptr;

  if (lgc::rt::getLgcRtShaderStage(Call->getFunction()) != lgc::rt::RayTracingShaderStage::KernelEntry) {
    ResumeAddr = Builder.create<cps::AsContinuationReferenceOp>(ResumeFun);
  } else {
    // For entry-point compute kernel, pass a poison %rcr.
    ResumeAddr = PoisonValue::get(Builder.getInt32Ty());
  }

  CR = Call->getArgOperand(0);
  TailArgs.append(SmallVector<Value *>(drop_begin(Call->args(), 3)));

  Value *ShaderIndex = Call->getArgOperand(2);
  Value *Level = Call->getArgOperand(1);
  uint32_t LevelImm = cast<ConstantInt>(Level)->getZExtValue();

  auto *Csp = PoisonValue::get(Builder.getInt32Ty());
  auto *JumpCall = Builder.create<cps::JumpOp>(CR, LevelImm, Csp, ShaderIndex, ResumeAddr, TailArgs);
  // Replace this instruction with a call to cps.jump.
  JumpCall->copyMetadata(*Call);

  // Remove instructions at the end of the block
  Builder.SetInsertPoint(Call);
  auto *Unreachable = Builder.CreateUnreachable();
  for (auto &I : make_early_inc_range(reverse(*JumpCall->getParent()))) {
    if (&I == Unreachable)
      break;
    I.eraseFromParent();
  }
}

/// Lower lgc.rt calls inside cps functions.
void CleanupContinuationsPassImpl::lowerIntrinsicCall(Function *F, ContinuationData &Data) {
  if (Data.CpsIntrinsicCalls.empty())
    return;

  auto Stage = lgc::rt::getLgcRtShaderStage(F);
  if (!Stage)
    return;

  CompilerUtils::CrossModuleInliner CrossInliner;
  Value *SystemDataArg = F->getArg(CpsArgIdx::SystemData);
  Type *SystemDataTy = SystemDataArg->getType();

  Builder.SetInsertPointPastAllocas(F);
  auto *SystemData = Builder.CreateAlloca(SystemDataTy);
  SystemData->setName("system.data.alloca");
  assert(SystemDataTy->isStructTy() && "SystemData should be struct type");

  Builder.CreateStore(SystemDataArg, SystemData);

  // All intrinsics that we need to inline are rematerializable/constant, the others have been inlined
  // by LowerRaytracingPipeline.
  // Therefore it is enough to inline every used intrinsic once at the start of the function. This
  // reduces the generated code size.

  // Map intrinsic function to value
  SmallDenseMap<Function *, Value *> CachedIntrinsics;

  while (!Data.CpsIntrinsicCalls.empty()) {
    // Ensure the list gets freed, since otherwise we will process the same calls twice by accident.
    auto *Call = Data.CpsIntrinsicCalls.pop_back_val();
    assert(Call->arg_empty() && "Expect only calls without arguments");
    Value *&Cached = CachedIntrinsics[Call->getCalledFunction()];
    if (Cached != nullptr) {
      Call->replaceAllUsesWith(Cached);
      Call->eraseFromParent();
    } else {
      Cached = replaceIntrinsicCall(Builder, SystemDataTy, SystemData, *Stage, Call, GpurtLibrary ? GpurtLibrary : &Mod,
                                    CrossInliner, true);
    }
  }
}

bool CleanupContinuationsPassImpl::handleIntrinsics(llvm::ModuleAnalysisManager &AnalysisManager) {
  bool Changed = false;

  for (auto &F : Mod.functions()) {
    auto Name = F.getName();
    if (Name.starts_with("_AmdValueI32Count")) {
      Changed = true;
      ContHelper::handleValueI32Count(F, Builder);
    } else if (Name.starts_with("_AmdValueGetI32")) {
      Changed = true;
      ContHelper::handleValueGetI32(F, Builder);
    } else if (Name.starts_with("_AmdValueSetI32")) {
      Changed = true;
      ContHelper::handleValueSetI32(F, Builder);
    } else if (F.getName().starts_with("_AmdContStack")) {
      Changed = true;

      auto &FAM = AnalysisManager.getResult<FunctionAnalysisManagerModuleProxy>(Mod).getManager();

      handleContStackIntrinsic(FAM, F);
    }
  }

  return Changed;
}

// Replace calls to _AmdContStack* with calls to lgc.cps dialect ops.
// Do some simple constant propagation on the fly.
void CleanupContinuationsPassImpl::handleContStackIntrinsic(FunctionAnalysisManager &FAM, Function &F) {
  // Check if the function is either of void return type or i32 return type and
  // has no arguments or a single integer argument dividable by 32 (to allow
  // storing and loading multiple dwords via AmdContStackLoad /
  // AmdContStackStore).
  Type *ReturnTy = F.getReturnType();
  (void)ReturnTy;
  assert((ReturnTy->isVoidTy() || (ReturnTy->isIntegerTy() && (ReturnTy->getIntegerBitWidth() % 32 == 0))) &&
         "CleanupContinuationsPassImpl::handleContStackIntrinsic: Invalid "
         "return type!");

  Type *FuncTy = F.getFunctionType();
  (void)(FuncTy);
  assert((FuncTy->getFunctionNumParams() == 0 || FuncTy->getFunctionParamType(0)->isIntegerTy()) &&
         "CleanupContinuationsPassImpl::handleContStackIntrinsic: Invalid "
         "argument signature!");

  StringRef FuncName = F.getName();
  FuncName.consume_front("_AmdContStack");

  auto ConstantFoldInstruction = [&](Function *Parent, Value *SizeArg) -> Value * {
    if (!isa<Instruction>(SizeArg))
      return SizeArg;

    if (auto *I = dyn_cast<Instruction>(SizeArg)) {
      // Do some basic constant-propagation
      // This is needed because this pass just replaced the ValueI32Count
      // and ContPayloadRegistersI32Count intrinsics and the allocated size
      // usually depends on these values.
      auto &DT = FAM.getResult<DominatorTreeAnalysis>(*Parent);
      auto &TLI = FAM.getResult<TargetLibraryAnalysis>(*Parent);
      auto &AC = FAM.getResult<AssumptionAnalysis>(*Parent);
      const SimplifyQuery SQ(Parent->getParent()->getDataLayout(), &TLI, &DT, &AC);

      if (auto *NewSize = simplifyInstruction(I, SQ))
        return NewSize;
    }

    return SizeArg;
  };

  llvm::forEachCall(F, [&](CallInst &CInst) {
    Value *Replacement = nullptr;
    Builder.SetInsertPoint(&CInst);

    Type *DestTy = CInst.getType();

    bool IsMemoryAccess = false;
    if (FuncName.starts_with("Alloc")) {
      Value *SizeArg = ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Replacement = Builder.create<lgc::cps::AllocOp>(SizeArg);

      if (auto *Size = dyn_cast<ConstantInt>(SizeArg))
        ContHelper::StackSize::inc(CInst.getFunction(), Size->getSExtValue());
    } else if (FuncName.starts_with("Free")) {
      Value *SizeArg = ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Replacement = Builder.create<lgc::cps::FreeOp>(SizeArg);
    } else if (FuncName.starts_with("SetPtr")) {
      Value *Vsp = CInst.getArgOperand(0);
      Replacement = Builder.create<lgc::cps::SetVspOp>(
          Builder.CreateIntToPtr(Vsp, PointerType::get(Builder.getInt8Ty(), lgc::cps::stackAddrSpace)));
    } else if (FuncName.starts_with("GetPtr")) {
      Replacement = Builder.create<lgc::cps::GetVspOp>();
    } else if (FuncName.starts_with("Load")) {
      Value *Addr = ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Value *Ptr = Builder.CreateIntToPtr(Addr, Builder.getPtrTy(lgc::cps::stackAddrSpace));
      Replacement = Builder.CreateAlignedLoad(DestTy, Ptr, Align(CpsStackLowering::getContinuationStackAlignment()));

      if (FuncName.starts_with("LoadLastUse"))
        CompilerUtils::setIsLastUseLoad(*cast<LoadInst>(Replacement));

      IsMemoryAccess = true;
    } else if (FuncName.starts_with("Store")) {
      assert(FuncTy->getFunctionNumParams() == 2 && "CleanupContinuationsPassImpl::handleContStackIntrinsic: Invalid "
                                                    "argument signature for AmdContStackStore!");

      Value *Addr = ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Value *Val = CInst.getArgOperand(1);
      Value *Ptr = Builder.CreateIntToPtr(Addr, Builder.getPtrTy(lgc::cps::stackAddrSpace));
      Builder.CreateAlignedStore(Val, Ptr, Align(CpsStackLowering::getContinuationStackAlignment()));

      IsMemoryAccess = true;
    } else {
      llvm_unreachable("CleanupContinuationsPassImpl::handleContStackIntrinsic: "
                       "Unknown intrinsic!");
    }

    if (Replacement) {
      if (!DestTy->isVoidTy() && !IsMemoryAccess)
        Replacement = Builder.CreatePtrToInt(Replacement, DestTy);

      CInst.replaceAllUsesWith(Replacement);
    }

    CInst.eraseFromParent();
  });
}

void CleanupContinuationsPassImpl::lowerGetResumePoint(Module &Mod) {
  for (auto &F : make_early_inc_range(Mod)) {
    auto FuncName = F.getName();
    if (!FuncName.starts_with("_AmdGetResumePointAddr"))
      continue;
    for (auto &Use : make_early_inc_range(F.uses())) {
      auto *GetResumeCall = dyn_cast<CallInst>(Use.getUser());
      // Get the lgc.cps.jump that is dominated by this _AmdGetResumePointAddr
      // call.
      auto JumpCall = findDominatedContinueCall(GetResumeCall);
      assert(JumpCall && "Should find a dominated call to lgc.cps.jump");
      lgc::cps::JumpOp *Jump = cast<cps::JumpOp>(*JumpCall);
      Value *ResumeFn = Jump->getRcr();
      assert(ResumeFn && isa<cps::AsContinuationReferenceOp>(ResumeFn));
      // We can always move this as.continuation.reference call.
      cast<Instruction>(ResumeFn)->moveBefore(GetResumeCall);
      Builder.SetInsertPoint(GetResumeCall);
      GetResumeCall->replaceAllUsesWith(ResumeFn);
      GetResumeCall->eraseFromParent();

      // Re-create the lgc.cps.jump call without the return address
      // argument, since the calling code handles it manually.
      SmallVector<Value *> Args;
      for (unsigned I = 0; I < Jump->arg_size(); I++) {
        if (I != 4) // Return address argument
          Args.push_back(Jump->getArgOperand(I));
      }

      Builder.SetInsertPoint(Jump);
      auto *NewCall = Builder.CreateCall(Jump->getCalledFunction(), Args);
      NewCall->copyMetadata(*Jump);

      Jump->eraseFromParent();
    }
  }
}

CleanupContinuationsPassImpl::CleanupContinuationsPassImpl(llvm::Module &M, llvm::Module &GpurtLibrary,
                                                           llvm::ModuleAnalysisManager &AM)
    : Mod(M), AnalysisManager(AM), Builder{Mod.getContext()}, ContMalloc{Mod.getFunction("continuation.malloc")},
      ContFree{Mod.getFunction("continuation.free")}, GpurtLibrary{&GpurtLibrary} {
}

llvm::PreservedAnalyses CleanupContinuationsPassImpl::run() {
  auto &FAM = AnalysisManager.getResult<FunctionAnalysisManagerModuleProxy>(Mod).getManager();

  // Map the entry function of a continuation to the analysis result
  for (auto &F : Mod.functions()) {
    if (F.empty())
      continue;
    if (auto *MD = F.getMetadata(ContHelper::MDContinuationName)) {
      analyzeContinuation(F, MD);
    } else if (lgc::rt::getLgcRtShaderStage(&F) == lgc::rt::RayTracingShaderStage::KernelEntry) {
      analyzeContinuation(F, nullptr);
    }
  }

  // Check if the continuation state is used in any function part
  for (auto &FuncData : ToProcess) {
    // Kernel entry functions do not have FuncData.MD and we do not need to
    // handle them here.
    if (!FuncData.second.MallocCall && FuncData.second.MD) {
      for (auto *F : FuncData.second.Functions) {
        // If this is the continuation start part.
        bool IsStart = (F == FuncData.first);
        Value *ContFrame;
        if (IsStart)
          ContFrame = F->getArg(F->arg_size() - 1);
        else
          ContFrame = F->getArg(0);
        if (!ContFrame->user_empty())
          FuncData.second.ContStateBytes = MinimumContinuationStateBytes;
      }
    }
  }

  // Erase store coroutine frame to make later continuation stack traversal
  // easy.
  for (auto &FuncData : ToProcess) {
    if (!FuncData.second.MallocCall)
      continue;
    auto *StartF = FuncData.first;
    auto *BufferArg = StartF->getArg(StartF->arg_size() - 1);
    auto *Store = cast<Instruction>(BufferArg->getUniqueUndroppableUser());
    Store->eraseFromParent();
  }

  // Try to do store->load forwarding here.
  for (auto &FuncData : ToProcess) {
    // Kernel entry functions do not have FuncData.MD and we do not need to
    // handle them here.
    if (FuncData.second.MD) {
      for (auto *F : FuncData.second.Functions) {
        auto &DT = FAM.getResult<DominatorTreeAnalysis>(*F);
        // If this is the continuation start part.
        bool IsStart = (F == FuncData.first);
        Value *ContFrame = getContinuationFramePtr(F, IsStart, FuncData.second);
        // Traversal the users to forward store to load instruction.
        forwardContinuationFrameStoreToLoad(DT, ContFrame);
      }
    }
  }

  bool Changed = false;
  if (!ToProcess.empty()) {
    auto StackAddrspaceMD = ContHelper::tryGetStackAddrspace(Mod);
    assert(StackAddrspaceMD.has_value() && "Missing continuation.stackAddrspace metadata");
    auto StackAddrspace = StackAddrspaceMD.value();

    if (StackAddrspace == ContStackAddrspace::Global)
      GetGlobalMemBase = getContinuationStackGlobalMemBase(*GpurtLibrary);

    StackLowering.emplace(Mod.getContext(), static_cast<unsigned>(StackAddrspace));

    processContinuations();

    lowerGetResumePoint(Mod);
    Changed = true;
  }

  Changed |= handleIntrinsics(AnalysisManager);

  // Run stack lowering
  for (auto &FuncData : ToProcess) {
    for (Function *F : FuncData.second.NewFunctions) {
      bool RequiresIncomingCsp = lgc::rt::getLgcRtShaderStage(F) != lgc::rt::RayTracingShaderStage::KernelEntry;
      StackLowering->lowerCpsStackOps(F, GetGlobalMemBase, RequiresIncomingCsp);
    }
  }

  Changed |= lowerCompleteOp(Mod);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace

llvm::PreservedAnalyses CleanupContinuationsPass::run(llvm::Module &Mod, llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the cleanup-continuations pass\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Mod);

  auto &GpurtContext = GpurtContext::get(Mod.getContext());
  Module &GpurtLibrary = GpurtContext.theModule ? *GpurtContext.theModule : Mod;
  CleanupContinuationsPassImpl Impl(Mod, GpurtLibrary, AnalysisManager);
  return Impl.run();
}
