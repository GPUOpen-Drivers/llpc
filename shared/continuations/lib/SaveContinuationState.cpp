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
// This pass replaces all uses of continuation.getContinuationStackOffset with a
// local variable and inits the stack pointer in entry functions with
// continuation.initialContinuationStackPtr.
//
// TODO: This pass used to handle a lot more regarding continuation state,
//       and now only lowering of the CSP remains. The pass is now poorly named,
//       and a later patch might completely remove this pass once CSP lowering
//       is moved elsewhere.
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

  if (auto *Intr = M.getFunction("continuation.getContinuationStackOffset")) {
    Changed = true;
    lowerCsp(Intr);
  }

  B = nullptr;

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
