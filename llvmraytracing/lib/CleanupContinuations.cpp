//===- CleanupContinuations.cpp - Post-process output of coroutine passes -===//
//
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
// 2. Replace @lgc.ilcps.return with simple `ret`, which means thread
//    termination.
// 3. Edit function signatures, like removing coroutine frame pointer argument,
//    adding needed ones (state, rcr, returned_values) for resume function.
// 4. Allocating/freeing cps stack space as needed.
//===----------------------------------------------------------------------===//

#include "compilerutils/CompilerUtils.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

using namespace llvm;
using namespace lgc;

#define DEBUG_TYPE "cleanup-continuations"

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

      assert(isa<CallInst>(std::get<1>(CallEntry)) && "Phi should come from a call");
      Result.insert(std::make_pair(PhiBB, std::make_pair(cast<CallInst>(std::get<1>(CallEntry)), ResumeFunEntry)));
    }
  } else {
    assert(isa<Constant>(ResumeFun) && "Resume function should be a constant function");
    assert(isa<CallInst>(Call) && "Call should be a CallInst");
    auto *CallI = cast<CallInst>(Call);
    Result.insert(std::make_pair(BB, std::make_pair(CallI, ResumeFun)));
  }
  return Result;
}

void CleanupContinuationsPass::analyzeContinuation(Function &F, MDNode *MD) {
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
    Data.ContStateBytes = cast<ConstantInt>(Data.MallocCall->getArgOperand(0))->getSExtValue();
  }
  if (Data.ContStateBytes > MaxContStateBytes)
    MaxContStateBytes = Data.ContStateBytes;
}

void CleanupContinuationsPass::updateCpsStack(Function *F, Function *NewFunc, bool IsStart, ContinuationData &CpsInfo) {

  Builder->SetInsertPoint(&*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
  Value *CpsStack = nullptr;
  if (IsStart) {
    CpsStack = Builder->create<cps::AllocOp>(Builder->getInt32(CpsInfo.ContStateBytes));
    CpsStack->setName("cont.state.stack.segment");
  } else {
    CpsStack = Builder->create<cps::PeekOp>(Builder->getInt32(CpsInfo.ContStateBytes));
  }

  SmallVector<Instruction *> ToBeRemoved;
  Value *OldBase = getContinuationFramePtr(F, IsStart, CpsInfo, &ToBeRemoved);
  CompilerUtils::replaceAllPointerUses(Builder, OldBase, CpsStack, ToBeRemoved);

  for (auto *I : reverse(ToBeRemoved))
    I->eraseFromParent();
}

static void updateCpsFunctionArgs(Function *OldFunc, Function *NewFunc, const SmallVector<Value *> &AllArgValues) {
  // Set arg names for new function
  for (unsigned Idx = 0; Idx != NewFunc->getFunctionType()->params().size(); ++Idx) {
    Argument *Arg = NewFunc->getArg(Idx);
    Value *OldVal = AllArgValues[Idx];
    if (OldVal) {
      Arg->setName(OldVal->getName());
      OldVal->replaceAllUsesWith(Arg);
    }
  }
}

static void buildCpsArgInfos(Function *F, bool IsStart, SmallVector<Type *> &AllArgTypes,
                             SmallVector<Value *> &AllArgValues, SmallVector<AttributeSet> &ParamAttrs,
                             SmallVector<Instruction *> &InstsToRemove) {

  auto &Context = F->getContext();
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
    //  Add extra arguments ({} %state, i32 %rcr, i32 %shader-index) for resume
    //  part. But for now, we always use continuation stack to pass continuation
    //  state.
    AllArgTypes.push_back(StructType::get(Context, {}));
    AllArgValues.push_back(nullptr);
    AllArgTypes.push_back(IntegerType::get(Context, 32));
    AllArgValues.push_back(nullptr);
    AllArgTypes.push_back(IntegerType::get(Context, 32));
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
}

/// Find the continuation state pointer, either returned by the malloc or
/// given as an argument
Value *CleanupContinuationsPass::getContinuationFramePtr(Function *F, bool IsStart,
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
void CleanupContinuationsPass::removeContFreeCall(Function *F, Function *ContFree) {
  for (auto *User : make_early_inc_range(ContFree->users())) {
    if (auto *Call = dyn_cast<CallInst>(User)) {
      if (Call->getFunction() == F) {
        Call->eraseFromParent();
        break;
      }
    }
  }
}

/// Insert cps.free() before the original function exits.
/// Note: we skip the cps.free() insertion before calls to
/// @lgc.ilcps.return. Because this is not useful any more as it means the
/// thread termination.
void CleanupContinuationsPass::freeCpsStack(Function *F, ContinuationData &CpsInfo) {
  struct VisitState {
    ContinuationData &CpsInfo;
    llvm_dialects::Builder *Builder;
    Function *F;
  };
  VisitState State = {CpsInfo, Builder, F};
  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitState>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
          .add<cps::JumpOp>([](auto &State, auto &Jump) {
            if (Jump.getFunction() == State.F && State.CpsInfo.ContStateBytes) {
              State.Builder->SetInsertPoint(&Jump);
              State.Builder->template create<cps::FreeOp>(State.Builder->getInt32(State.CpsInfo.ContStateBytes));
            }
          })
          .build();
  Visitor.visit(State, *F);
}

void CleanupContinuationsPass::processContinuations() {
  // Summarize of what to do here:
  // 1. Continuation Stack
  //    a.) cps.alloc() in start, and cps.peek() cps.free() in resume.
  //    b.) change the address space for cps stack to 32.
  // 2. prepare arguments passed to cps.jump and insert the call at the exit of
  //    start part.
  // 3. Edit resume signature to add the state/rcr/shader-indxe/returnvalues.
  for (auto &FuncData : ToProcess) {
    LLVM_DEBUG(dbgs() << "Processing function: " << FuncData.first->getName() << "\n");
    for (auto *F : FuncData.second.Functions) {
      // Set same linkage as for start function
      if (F != FuncData.first)
        F->setLinkage(FuncData.first->getLinkage());

      // Ignore the stub created for the coroutine passes
      if (F->empty())
        continue;

      LLVM_DEBUG(dbgs() << "Processing function part: " << F->getName() << "\n");

      // If this is the continuation start
      bool IsStart = F == FuncData.first;
      // We don't need to touch resume part of non-cps function, this usually
      // should be entry-point compute kernel. The resume part will be erased
      // at the end.
      if (!IsStart && !cps::isCpsFunction(*F))
        continue;

      SmallVector<Type *> AllArgTypes;
      SmallVector<Value *> AllArgValues;
      SmallVector<AttributeSet> ParamAttrs;
      SmallVector<Instruction *> InstsToRemove;

      buildCpsArgInfos(F, IsStart, AllArgTypes, AllArgValues, ParamAttrs, InstsToRemove);

      if (ContFree)
        removeContFreeCall(F, ContFree);

      // Create new empty function
      if (FuncData.second.MD)
        F->eraseMetadata(FuncData.second.MD->getMetadataID());
      auto &Context = F->getContext();
      auto *NewFuncTy = FunctionType::get(Type::getVoidTy(Context), AllArgTypes, false);
      Function *NewFunc = CompilerUtils::cloneFunctionHeader(*F, NewFuncTy, ParamAttrs);
      NewFunc->takeName(F);
      FuncData.second.NewFunctions.push_back(NewFunc);

      // Transfer code from old function to new function
      llvm::moveFunctionBody(*F, *NewFunc);

      auto &CpsInfo = FuncData.second;
      if (CpsInfo.ContStateBytes)
        updateCpsStack(F, NewFunc, IsStart, CpsInfo);

      updateCpsFunctionArgs(F, NewFunc, AllArgValues);

      freeCpsStack(NewFunc, CpsInfo);
      // Handle the function returns
      for (auto &BB : make_early_inc_range(*NewFunc)) {
        auto *I = BB.getTerminator();
        if (isa<ReturnInst>(I)) {
          handleContinue(FuncData.second, I);
        } else if (I->getOpcode() == Instruction::Unreachable) {
          // We should only possibly have 'lgc.ilcps.return' or
          // 'lgc.cps.jump' call before unreachable.
          auto *Call = cast<CallInst>(--I->getIterator());
          if (isa<lgc::ilcps::ReturnOp>(Call)) {
            Builder->SetInsertPoint(Call);
            Builder->CreateRetVoid();
            Call->eraseFromParent();
            I->eraseFromParent();
          } else {
            assert(isa<cps::JumpOp>(*Call));
          }
        }
      }

      for (auto *I : InstsToRemove)
        I->eraseFromParent();

      // Replace the old function with the new one.
      F->replaceAllUsesWith(NewFunc);
      // Update the `ToProcess` for later processing.
      if (IsStart)
        FuncData.first = NewFunc;
    }
  }

  // Remove the old functions
  for (auto &FuncData : ToProcess) {
    if (FuncData.second.Functions.size() > 1) {
      // Only for functions that were split
      for (auto *F : FuncData.second.Functions)
        F->eraseFromParent();
    }
  }
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
///   call void (...) @lgc.cps.jump(i32 %cr, i32 2, {} poison,
///                                 i32 %cr2, ...)
///
/// Also handles cases where the token and resume function are behind a phi.
void CleanupContinuationsPass::handleContinue(ContinuationData &Data, Instruction *Ret) {
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

void CleanupContinuationsPass::handleSingleContinue(ContinuationData &Data, CallInst *Call, Value *ResumeFun) {
  Builder->SetInsertPoint(Call);

  SmallVector<Value *> TailArgs;
  uint32_t SkipCount = 2;
  // WaitMask and %rcr (aka. return continuation reference) for the callee.
  if (cps::isCpsFunction(*cast<Function>(ResumeFun))) {
    // Ensure the first argument stays the wait mask. This comes after the CR
    // and the levels.
    if (ContHelper::isWaitAwaitCall(*Call)) {
      TailArgs.push_back(Call->getArgOperand(2));
      ++SkipCount;
    }

    auto *ResumeCR = Builder->create<cps::AsContinuationReferenceOp>(ContinuationReferenceType, ResumeFun);

    TailArgs.push_back(ResumeCR);
  } else {
    // For entry-point compute kernel, pass a poison %rcr.
    TailArgs.push_back(PoisonValue::get(Builder->getInt32Ty()));
  }
  // Skip continuation.reference, levels and potentially the wait mask.
  TailArgs.append(SmallVector<Value *>(drop_begin(Call->args(), SkipCount)));
  auto *CR = Call->getArgOperand(0);
  Value *Level = Call->getArgOperand(ContHelper::isWaitAwaitCall(*Call) ? 2 : 1);
  unsigned LevelImm = cast<ConstantInt>(Level)->getZExtValue();

  // TODO: Continuation state are passed through stack for now.
  auto *State = PoisonValue::get(StructType::get(Builder->getContext(), {}));
  auto *JumpCall = Builder->create<cps::JumpOp>(CR, LevelImm, State, TailArgs);
  // Replace this instruction with a call to cps.jump.
  JumpCall->copyMetadata(*Call);

  // Remove instructions at the end of the block
  Builder->SetInsertPoint(Call);
  auto *Unreachable = Builder->CreateUnreachable();
  for (auto &I : make_early_inc_range(reverse(*JumpCall->getParent()))) {
    if (&I == Unreachable)
      break;
    I.eraseFromParent();
  }
}

/// Lower lgc.rt calls inside cps functions.
void CleanupContinuationsPass::lowerIntrinsicCall(Module &Mod) {
  DenseMap<Function *, SmallVector<CallInst *>> CpsIntrinsicCalls;

  // We only care about lgc.rt here.
  for (auto &F : Mod.functions()) {
    auto Name = F.getName();
    if (!Name.starts_with("lgc.rt"))
      continue;

    llvm::forEachCall(F, [&](CallInst &CInst) {
      auto IntrImplEntry = llvm::findIntrImplEntryByIntrinsicCall(&CInst);
      if (IntrImplEntry == std::nullopt)
        return;

      auto *Caller = CInst.getFunction();
      CpsIntrinsicCalls[Caller].push_back(&CInst);
    });
  }

  CompilerUtils::CrossModuleInliner CrossInliner;
  for (const auto &[Caller, IntrinsicCalls] : CpsIntrinsicCalls) {
    // No need to insert system data alloca if no intrinsic call.
    if (IntrinsicCalls.empty())
      continue;

    auto Stage = lgc::rt::getLgcRtShaderStage(Caller);
    if (!Stage)
      continue;

    // Signature of cps function: { state, rcr, shader-index, system-data}
    auto *SystemDataArg = Caller->getArg(CpsArgIdxSystemData);
    assert(SystemDataArg->getType()->isStructTy() && "SystemData should be struct type");
    auto *AllocaInsertPt = &*Caller->getEntryBlock().getFirstNonPHIOrDbgOrAlloca();
    Builder->SetInsertPoint(AllocaInsertPt);
    auto *SystemData = Builder->CreateAlloca(SystemDataArg->getType());
    Builder->CreateStore(SystemDataArg, SystemData);
    for (auto *Call : IntrinsicCalls)
      replaceIntrinsicCall(*Builder, SystemDataArg->getType(), SystemData, *Stage, Call,
                           GpurtLibrary ? GpurtLibrary : &Mod, CrossInliner);
  }
}

void CleanupContinuationsPass::lowerGetResumePoint(Module &Mod) {
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
      // For wait calls, skip the wait mask.
      uint32_t SkipCount = ContHelper::isWaitAwaitCall(*(JumpCall.value())) ? 1 : 0;

      lgc::cps::JumpOp *Jump = cast<cps::JumpOp>(*JumpCall);
      Value *ResumeFn = *(Jump->getTail().begin() + SkipCount);
      assert(ResumeFn && isa<cps::AsContinuationReferenceOp>(ResumeFn));
      // We can always move this as.continuation.reference call.
      cast<Instruction>(ResumeFn)->moveBefore(GetResumeCall);
      Builder->SetInsertPoint(GetResumeCall);
      auto *ResumePtr = Builder->CreateZExt(ResumeFn, Builder->getInt64Ty());
      GetResumeCall->replaceAllUsesWith(ResumePtr);
      GetResumeCall->eraseFromParent();
    }
  }
}

llvm::PreservedAnalyses CleanupContinuationsPass::run(llvm::Module &Mod, llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the lgc-cleanup-continuations pass\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Mod);
  auto &FAM = AnalysisManager.getResult<FunctionAnalysisManagerModuleProxy>(Mod).getManager();

  ToProcess.clear();
  MaxContStateBytes = 0;
  ContMalloc = Mod.getFunction("continuation.malloc");
  ContFree = Mod.getFunction("continuation.free");
  GpurtLibrary = GpurtContext::get(Mod.getContext()).theModule;

  llvm_dialects::Builder B(Mod.getContext());
  Builder = &B;

  if (Use64BitContinuationReferences)
    ContinuationReferenceType = Builder->getInt64Ty();
  else
    ContinuationReferenceType = Builder->getInt32Ty();

  // Map the entry function of a continuation to the analysis result
  for (auto &F : Mod.functions()) {
    if (F.empty())
      continue;
    if (auto *MD = F.getMetadata(ContHelper::MDContinuationName))
      analyzeContinuation(F, MD);
  }

  // Check if the continuation state is used in any function part
  for (auto &FuncData : ToProcess) {
    if (!FuncData.second.MallocCall) {
      for (auto *F : FuncData.second.Functions) {
        // If this is the continuation start part.
        bool IsStart = (F == FuncData.first);
        Value *ContFrame;
        if (IsStart)
          ContFrame = F->getArg(F->arg_size() - 1);
        else
          ContFrame = F->getArg(0);
        if (!ContFrame->user_empty()) {
          FuncData.second.ContStateBytes = MinimumContinuationStateBytes;
          if (MinimumContinuationStateBytes > MaxContStateBytes)
            MaxContStateBytes = MinimumContinuationStateBytes;
        }
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
    for (auto *F : FuncData.second.Functions) {
      auto &DT = FAM.getResult<DominatorTreeAnalysis>(*F);
      // If this is the continuation start part.
      bool IsStart = (F == FuncData.first);
      Value *ContFrame = getContinuationFramePtr(F, IsStart, FuncData.second);
      // Traversal the users to forward store to load instruction.
      forwardContinuationFrameStoreToLoad(DT, ContFrame);
    }
  }

  if (!ToProcess.empty()) {
    processContinuations();
    // Lower lgc.rt intrinsics
    lowerIntrinsicCall(Mod);

    lowerGetResumePoint(Mod);
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}
