/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- LgcCpsJumpInliner.cpp - Inline lgc.cps.jump and continue calls -===//
//
// A pass that inlines lgc.cps.jump calls with constant jump targets which reside in the GpuRt module.
//
//===----------------------------------------------------------------------===//

#include "compilerutils/CompilerUtils.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/LgcCpsDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"

#define DEBUG_TYPE "lgc-cps-jump-inliner"

namespace {
using namespace llvm;
using namespace lgc::cps;

class LgcCpsJumpInlinerPassImpl final {
public:
  LgcCpsJumpInlinerPassImpl(Module &M, Module &GpurtLibrary);
  PreservedAnalyses run();

private:
  Module *Mod;
  Module *GpurtLibrary;
  LLVMContext *Context;
  const DataLayout *DL;
  llvm_dialects::Builder Builder;
  CompilerUtils::CrossModuleInliner CrossInliner;
};
} // namespace

LgcCpsJumpInlinerPassImpl::LgcCpsJumpInlinerPassImpl(Module &M, Module &GpurtLibrary)
    : Mod{&M}, GpurtLibrary{&GpurtLibrary}, Context{&M.getContext()}, DL{&M.getDataLayout()}, Builder{
                                                                                                  Mod->getContext()} {
}

PreservedAnalyses LgcCpsJumpInlinerPassImpl::run() {
  using JumpVecTy = SmallVector<JumpOp *>;
  static const auto Visitor =
      llvm_dialects::VisitorBuilder<SmallVector<JumpOp *>>()
          .add<JumpOp>([](SmallVector<JumpOp *> &AllJumps, JumpOp &Jump) { AllJumps.push_back(&Jump); })
          .build();

  JumpVecTy AllJumps;
  // Collect lgc.cps.jump ops.
  Visitor.visit(AllJumps, *Mod);

  bool Changed = false;
  // Iterate over all collected jumps and try to inline the jump target.
  for (auto *Jump : AllJumps) {
    auto *AsCROp = dyn_cast<AsContinuationReferenceOp>(Jump->getTarget());
    if (!AsCROp)
      continue;

    Function *JumpTargetFunc = cast<Function>(AsCROp->getFn());

    JumpTargetFunc = GpurtLibrary->getFunction(JumpTargetFunc->getName());

    assert(JumpTargetFunc && !JumpTargetFunc->isDeclaration());

    Builder.SetInsertPoint(Jump);
    SmallVector<Value *> ArgList;
    assert(Jump->getState()->getType()->isEmptyTy());

    if (isCpsFunction(*JumpTargetFunc)) {
      // TODO: We need to ensure we properly pass in RCR and shader index.
      ArgList.push_back(Jump->getState());
    }

    ArgList.append(Jump->getTail().begin(), Jump->getTail().end());

    CrossInliner.inlineCall(Builder, JumpTargetFunc, ArgList);

    // Cleanup work.
    Jump->eraseFromParent();

    if (AsCROp->user_empty())
      AsCROp->eraseFromParent();

    // There might still be other users left, if the function is not referenced as direct jump target.
    if (JumpTargetFunc->user_empty() && JumpTargetFunc->getLinkage() == GlobalValue::InternalLinkage)
      JumpTargetFunc->eraseFromParent();

    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses LgcCpsJumpInlinerPass::run(Module &Module, ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass lgc-cps-jump-inliner\n");

  auto &GpurtContext = lgc::GpurtContext::get(Module.getContext());
  LgcCpsJumpInlinerPassImpl Impl(Module, GpurtContext.theModule ? *GpurtContext.theModule : Module);

  return Impl.run();
}
