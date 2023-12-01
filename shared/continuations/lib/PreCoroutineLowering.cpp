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

//===- PreCoroutineLowering.cpp - Split BB for rematerialized code --------===//
//
// A pass that splits the BB after a TraceRay/CallShader/ReportHit call.
// That moves all rematerialized code after the inlined TraceRay/etc. and
// ensures that the local root index is set before it is accessed.
//
// Also removes already inline driver functions that are not needed anymore.
//
// Also lowers the GetShaderKind() and GetCurrentFuncAddr() intrinsics which is
// now possible that driver functions have been inlined.
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

#define DEBUG_TYPE "pre-coroutine-lowering"

PreCoroutineLoweringPass::PreCoroutineLoweringPass() {}

// Split BB after _AmdRestoreSystemData.
// The coroutine passes rematerialize to the start of the basic block of a use.
// We split the block so that every rematerialized dxil intrinsic lands after
// the restore call and accesses the restored system data.
// If we did not do that, an intrinsic that is rematerialized to before
// RestoreSystemData is called gets an uninitialized system data struct as
// argument.
bool PreCoroutineLoweringPass::splitBB() {
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

bool PreCoroutineLoweringPass::removeInlinedIntrinsics() {
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
PreCoroutineLoweringPass::run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pre-coroutine-lowering pass\n");

  Mod = &M;

  bool Changed = splitBB();

  // Remove already inlined driver functions
  Changed |= removeInlinedIntrinsics();

  Changed |= lowerGetShaderKind();
  Changed |= lowerGetCurrentFuncAddr();

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

bool PreCoroutineLoweringPass::lowerGetShaderKind() {
  auto *GetShaderKind = Mod->getFunction("_AmdGetShaderKind");

  if (!GetShaderKind)
    return false;

  assert(GetShaderKind->getReturnType()->isIntegerTy(32) &&
         GetShaderKind->arg_size() == 0);

  if (!GetShaderKind->use_empty()) {
    for (auto &Use : make_early_inc_range(GetShaderKind->uses())) {
      auto *CInst = dyn_cast<CallInst>(Use.getUser());

      if (!CInst || !CInst->isCallee(&Use)) {
        // Non-call use. This will likely result in a remaining non-lowered use
        // reported as error at the end of this function.
        continue;
      }

      Function *F = CInst->getFunction();
      auto Stage = lgc::rt::getLgcRtShaderStage(F);

      // Ignore GetShaderKind calls where we cannot find the shader kind.
      // This happens e.g. in gpurt-implemented intrinsics that got inlined,
      // but not removed.
      if (!Stage)
        continue;

      DXILShaderKind ShaderKind =
          DXILContHelper::shaderStageToDxilShaderKind(*Stage);
      auto *ShaderKindVal = ConstantInt::get(GetShaderKind->getReturnType(),
                                             static_cast<uint64_t>(ShaderKind));
      CInst->replaceAllUsesWith(ShaderKindVal);
      CInst->eraseFromParent();
    }
  }

  return true;
}

bool PreCoroutineLoweringPass::lowerGetCurrentFuncAddr() {
  auto *GetCurrentFuncAddr = Mod->getFunction("_AmdGetCurrentFuncAddr");

  if (!GetCurrentFuncAddr)
    return false;

  assert(GetCurrentFuncAddr->arg_size() == 0 &&
         // Returns an i32 or i64
         (GetCurrentFuncAddr->getReturnType()->isIntegerTy(32) ||
          GetCurrentFuncAddr->getReturnType()->isIntegerTy(64)));

  for (auto &Use : make_early_inc_range(GetCurrentFuncAddr->uses())) {
    auto *CInst = dyn_cast<CallInst>(Use.getUser());

    if (!CInst || !CInst->isCallee(&Use)) {
      // Non-call use. This will likely result in a remaining non-lowered use
      // reported as error at the end of this function.
      continue;
    }

    auto *FuncPtrToInt = ConstantExpr::getPtrToInt(
        CInst->getFunction(), GetCurrentFuncAddr->getReturnType());
    CInst->replaceAllUsesWith(FuncPtrToInt);
    CInst->eraseFromParent();
  }

  if (!GetCurrentFuncAddr->use_empty())
    report_fatal_error("Unknown uses of GetCurrentFuncAddr remain!");

  // Delete the declaration of the intrinsic after lowering, as future calls to
  // it are invalid.
  GetCurrentFuncAddr->eraseFromParent();

  return true;
}
