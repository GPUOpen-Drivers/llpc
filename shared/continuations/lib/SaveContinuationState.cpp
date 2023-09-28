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

//===- SaveContinuationState.cpp - Callee-save continuation state ---------===//
//
// Save and restore the overwritten global continuation state.
//
// This pass lowers the continuation.save/restore.continuation_state intrinsics.
// It also replaces all uses of continuation.getContinuationStackOffset with a
// local variable and inits the stack pointer in entry functions with
// continuation.initialContinuationStackPtr.
// The registerbuffer.getpointer(@CONTINUATION_STATE) calls are replaced with
// csp - (<cont state size> - <cont state register size>).
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "save-continuation-state"

SaveContinuationStatePass::SaveContinuationStatePass() {}

static uint32_t getContStateSaveSize(Function *F) {
  // Get entry function
  auto *MD = F->getMetadata(DXILContHelper::MDContinuationName);
  assert(MD && "Functions that call continuation state intrinsics need "
               "continuation metadata");
  auto *MDTup = cast<MDTuple>(MD);
  auto *EntryF = mdconst::extract<Function>(MDTup->getOperand(0));

  auto OptResult = DXILContHelper::tryGetContinuationStateByteCount(*EntryF);
  assert(OptResult.has_value() &&
         "Continuation entry functions need continuation.state metadata");
  return OptResult.value();
}

static uint32_t getContStateRegisterSize(Function *F) {
  uint32_t ContStateSize = getContStateSaveSize(F);
  return std::min(ContStateSize,
                  ContinuationStateRegisterCount * RegisterBytes);
}

static int64_t getContStateStackSize(Function *F) {
  int64_t ContStateSize = getContStateSaveSize(F);
  return std::max(ContStateSize -
                      (ContinuationStateRegisterCount * RegisterBytes),
                  static_cast<int64_t>(0));
}

bool SaveContinuationStatePass::lowerCalls(Function *Intr, bool IsSave) {
  SmallVector<CallInst *> ToProcess;
  for (auto *U : Intr->users()) {
    if (auto *Inst = dyn_cast<CallInst>(U))
      ToProcess.push_back(Inst);
  }

  for (auto *Call : ToProcess) {
    B->SetInsertPoint(Call);
    auto *F = Call->getFunction();
    auto SaveSize = getContStateRegisterSize(F);
    auto SaveI32s = divideCeil(SaveSize, RegisterBytes);
    uint64_t NeededStackSize =
        SaveI32s * (IsSave ? RegisterBytes : -RegisterBytes);
    auto Csps = moveContinuationStackOffset(*B, NeededStackSize);
    auto *Offset = IsSave ? Csps.first : Csps.second;
    auto *I32Mem = continuationStackOffsetToPtr(*B, Offset);

    for (unsigned I = 0; I < SaveI32s; I++) {
      // Copy ceil(SaveSize / RegisterSize) i32s from the global continuation
      // state into scratch Increase CSP
      auto *Reg = B->CreateGEP(ContState->getValueType(), ContState,
                               {B->getInt32(0), B->getInt32(I)});
      auto *Mem = B->CreateGEP(I32, I32Mem, B->getInt32(I));
      auto *Val = B->CreateLoad(I32, IsSave ? Reg : Mem);
      B->CreateStore(Val, IsSave ? Mem : Reg);
    }

    if (IsSave) {
      // Add to continuation stack size metadata
      DXILContHelper::addStackSize(F, NeededStackSize);
    }

    Call->eraseFromParent();
  }

  return !ToProcess.empty();
}

bool SaveContinuationStatePass::lowerContStateGetPointer() {
  bool Changed = false;
  auto *CspType = getContinuationStackOffsetType(Mod->getContext());
  for (auto &F : Mod->functions()) {
    if (F.getName().startswith("registerbuffer.setpointerbarrier")) {
      // Remove setpointerbarrier instructions related to continuation state
      for (auto &Use : make_early_inc_range(F.uses())) {
        if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
          if (CInst->isCallee(&Use)) {
            if (!isCastGlobal(ContState, CInst->getOperand(0)))
              continue;
            Changed = true;
            CInst->eraseFromParent();
          }
        }
      }
    } else if (F.getName().startswith("registerbuffer.getpointer")) {
      // Check calls that take the continuation state as argument
      for (auto &Use : make_early_inc_range(F.uses())) {
        if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
          if (CInst->isCallee(&Use)) {
            if (!isCastGlobal(ContState, CInst->getOperand(0)))
              continue;
            Changed = true;

            // Replace call with csp - <cont state size>
            B->SetInsertPoint(CInst);
            auto *CspOffsetPtr =
                B->CreateCall(getContinuationStackOffset(*Mod));
            auto *Offset = B->CreateLoad(CspType, CspOffsetPtr);
            auto *Ptr = continuationStackOffsetToPtr(*B, Offset);
            int64_t SaveSize = getContStateStackSize(CInst->getFunction());
            Value *Replacement =
                B->CreateGEP(B->getInt8Ty(), Ptr, B->getInt64(-SaveSize));
            Replacement =
                B->CreateBitOrPointerCast(Replacement, CInst->getType());
            CInst->replaceAllUsesWith(Replacement);
            CInst->eraseFromParent();
          }
        }
      }
    }
  }
  return Changed;
}

void SaveContinuationStatePass::lowerCsp(Function *Intr) {
  DenseMap<Function *, SmallVector<CallInst *>> ToProcess;
  for (auto *U : Intr->users()) {
    if (auto *Inst = dyn_cast<CallInst>(U)) {
      auto *F = Inst->getFunction();
      ToProcess[F].push_back(Inst);
    }
  }

  for (const auto &P : ToProcess) {
    auto *F = P.first;
    B->SetInsertPointPastAllocas(F);
    auto *CspType = getContinuationStackOffsetType(F->getContext());
    auto *Csp = B->CreateAlloca(CspType);
    Csp->setName("csp");
    bool IsEntry = F->hasMetadata(DXILContHelper::MDEntryName);
    if (IsEntry) {
      // Init csp through intrinsic
      auto *Init = getContinuationCspInit(*F->getParent());
      B->CreateStore(B->CreateCall(Init), Csp);
    } else {
      // Init csp from first argument
      B->CreateStore(F->getArg(0), Csp);
    }

    for (auto *Call : P.second) {
      Call->replaceAllUsesWith(Csp);
      Call->eraseFromParent();
    }
  }
}

llvm::PreservedAnalyses
SaveContinuationStatePass::run(llvm::Module &M,
                               llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the save-continuation-state pass\n");

  bool Changed = false;

  IRBuilder<> Builder(M.getContext());
  I32 = Builder.getInt32Ty();
  B = &Builder;
  Mod = &M;
  ContState = M.getGlobalVariable(DXILContHelper::GlobalContStateName);
  if (ContState)
    assert(ContState->getValueType()->getArrayNumElements() ==
               ContinuationStateRegisterCount &&
           "global continuation state has an unexpected size");

  Changed |= lowerContStateGetPointer();

  if (auto *Intr = M.getFunction("continuation.save.continuation_state"))
    Changed |= lowerCalls(Intr, true);

  if (auto *Intr = M.getFunction("continuation.restore.continuation_state"))
    Changed |= lowerCalls(Intr, false);

  if (auto *Intr = M.getFunction("continuation.getContinuationStackOffset")) {
    Changed = true;
    lowerCsp(Intr);
  }

  B = nullptr;

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
