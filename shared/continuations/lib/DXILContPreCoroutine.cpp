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

//===- DXILContPreCoroutine.cpp - Split BB for rematerialized code --------===//
//
// A pass that splits the BB after a TraceRay/CallShader/ReportHit call.
// That moves all rematerialized code after the inlined TraceRay/etc. and
// ensures that the local root index is set before it is accessed.
//
// Also removes already inline driver functions that are not needed anymore.
//
// Also lowers the GetShaderKind() intrinsic which is now possible that driver
// functions have been inlined.
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "dxil-cont-pre-coroutine"

DXILContPreCoroutinePass::DXILContPreCoroutinePass() {}

// Split BB after _AmdRestoreSystemData.
// The coroutine passes rematerialize to the start of the basic block of a use.
// We split the block so that every rematerialized dxil intrinsic lands after
// the restore call and accesses the restored system data.
// If we did not do that, an intrinsic that is rematerialized to before
// RestoreSystemData is called gets an uninitialized system data struct as
// argument.
bool DXILContPreCoroutinePass::splitBB() {
  bool Changed = false;
  for (auto &F : *Mod) {
    if (F.getName().startswith("_AmdRestoreSystemData")) {
      for (auto &Use : make_early_inc_range(F.uses())) {
        if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
          if (CInst->isCallee(&Use)) {
            auto *Next = &*++CInst->getIterator();
            CInst->eraseFromParent();
            if (Next->isTerminator())
              continue;
            SplitBlock(Next->getParent(), Next);
          }
        }
      }
      Changed = true;
    }
  }
  return Changed;
}

bool DXILContPreCoroutinePass::removeInlinedIntrinsics() {
  bool Changed = false;

  // Remove functions
  for (auto &F : make_early_inc_range(*Mod)) {
    auto Name = F.getName();
    // TODO Temporarily support multiple prefixes for TraceRay
    if (Name.startswith("amd.dx.TraceRay") ||
        Name.startswith("_cont_TraceRay") ||
        Name.startswith("_cont_CallShader") ||
        Name.startswith("_cont_ReportHit")) {
      F.eraseFromParent();
      Changed = true;
    }
  }
  return Changed;
}

llvm::PreservedAnalyses
DXILContPreCoroutinePass::run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the dxil-cont-pre-coroutine pass\n");

  Mod = &M;

  bool Changed = splitBB();

  // Remove already inlined driver functions
  Changed |= removeInlinedIntrinsics();

  Changed |= lowerGetShaderKind();

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

bool DXILContPreCoroutinePass::lowerGetShaderKind() {
  auto *GetShaderKind = Mod->getFunction("_AmdGetShaderKind");

  if (!GetShaderKind)
    return false;

  assert(GetShaderKind->getReturnType()->isIntegerTy(32) &&
         GetShaderKind->arg_size() == 0);

  if (!GetShaderKind->use_empty()) {
    MapVector<Function *, DXILShaderKind> ShaderKinds;
    analyzeShaderKinds(*Mod, ShaderKinds);

    for (auto &Use : make_early_inc_range(GetShaderKind->uses())) {
      auto *CInst = dyn_cast<CallInst>(Use.getUser());

      if (!CInst || !CInst->isCallee(&Use)) {
        // Non-call use. This will likely result in a remaining non-lowered use
        // reported as error at the end of this function.
        continue;
      }

      Function *F = CInst->getFunction();
      auto ShaderKindIt = ShaderKinds.find(F);

      // Ignore GetShaderKind calls where we cannot find the shader kind.
      // This happens e.g. in gpurt-implemented intrinsics that got inlined,
      // but not removed.
      if (ShaderKindIt == ShaderKinds.end())
        continue;

      DXILShaderKind ShaderKind = ShaderKindIt->second;
      auto *ShaderKindVal = ConstantInt::get(GetShaderKind->getReturnType(),
                                             static_cast<uint64_t>(ShaderKind));
      CInst->replaceAllUsesWith(ShaderKindVal);
      CInst->eraseFromParent();
    }
  }

  return true;
}
