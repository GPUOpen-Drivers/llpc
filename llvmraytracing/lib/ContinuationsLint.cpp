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

//===- ContinuationsLint.cpp - Continuations linter pass ------------------------===//
//
// This file implements a pass that runs some common integrity checks on a continuations module.
// This also runs the default LLVM linter on the whole module.
//===----------------------------------------------------------------------===//

#include "llvmraytracing/Continuations.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

#define DEBUG_TYPE "continuations-lint"

static const char ContLintAbortOnErrorArgName[] = "cont-lint-abort-on-error";
// Defaults to true. If the continuations module is broken at some point, then we cannot ignore that.
static cl::opt<bool> ContLintAbortOnError(ContLintAbortOnErrorArgName, cl::init(true),
                                          cl::desc("In the Continuations lint pass, abort on errors."));

#define Check(C, ...)                                                                                                  \
  do {                                                                                                                 \
    if (!(C)) {                                                                                                        \
      checkFailed(__VA_ARGS__);                                                                                        \
      return;                                                                                                          \
    }                                                                                                                  \
  } while (false)

namespace {
class ContinuationsLintPassImpl final {
public:
  ContinuationsLintPassImpl(Module &M);
  void run();

private:
  Module &Mod;

  using JumpVecTy = SmallVector<CallInst *>;
  JumpVecTy AllJumps;
  void collectJumps();
  void checkJumpTargets();
  void checkSetLocalRootIndex();

  // Printing and check logic borrowed from llvm's @Lint pass.
  std::string Messages;
  raw_string_ostream MessagesStr;
  /// A check failed, so printout out the condition and the message.
  ///
  /// This provides a nice place to put a breakpoint if you want to see why
  /// something is not correct.
  void checkFailed(const Twine &Message) { MessagesStr << Message << '\n'; }

  void writeValues(ArrayRef<const Value *> Vs) {
    for (const Value *V : Vs) {
      if (!V)
        continue;
      if (isa<Instruction>(V)) {
        MessagesStr << *V << '\n';
      } else {
        V->printAsOperand(MessagesStr, true, &Mod);
        MessagesStr << '\n';
      }
    }
  }

  template <typename T1, typename... Ts> void checkFailed(const Twine &Message, const T1 &V1, const Ts &...Vs) {
    checkFailed(Message);
    writeValues({V1, Vs...});
  }
};
} // anonymous namespace

ContinuationsLintPassImpl::ContinuationsLintPassImpl(Module &M) : Mod{M}, MessagesStr(Messages) {
}

void ContinuationsLintPassImpl::run() {
  LLVM_DEBUG(dbgs() << "Run the pass continuations-lint\n");
  collectJumps();

  checkJumpTargets();
  checkSetLocalRootIndex();

  dbgs() << MessagesStr.str();
  if (ContLintAbortOnError && !MessagesStr.str().empty())
    report_fatal_error(Twine("Continuations linter found errors, aborting. (enabled by --") +
                           ContLintAbortOnErrorArgName + ")",
                       false);
}

void ContinuationsLintPassImpl::collectJumps() {
  static const auto Visitor = llvm_dialects::VisitorBuilder<JumpVecTy>()
                                  .addSet<lgc::ilcps::ContinueOp, lgc::ilcps::WaitContinueOp, lgc::cps::JumpOp>(
                                      [](JumpVecTy &Jumps, Instruction &Op) { Jumps.push_back(cast<CallInst>(&Op)); })
                                  .build();

  Visitor.visit(AllJumps, Mod);
}

// Check that every possible jump candidate has a valid jump target
void ContinuationsLintPassImpl::checkJumpTargets() {
  for (auto *JumpCandidate : AllJumps) {
    Value *JumpTarget = nullptr;
    if (auto *Continue = dyn_cast<lgc::ilcps::ContinueOp>(JumpCandidate))
      JumpTarget = Continue->getShaderAddr();
    else if (auto *WaitContinue = dyn_cast<lgc::ilcps::WaitContinueOp>(JumpCandidate))
      JumpTarget = WaitContinue->getShaderAddr();
    else if (auto *Jump = dyn_cast<lgc::cps::JumpOp>(JumpCandidate))
      JumpTarget = Jump->getTarget();

    assert(JumpTarget);

    Check(!isa<UndefValue>(JumpTarget), "Jump has undefined jump target", JumpCandidate);
  }
}

// Check that every function has at most one setLocalRootIndex call.
void ContinuationsLintPassImpl::checkSetLocalRootIndex() {
  if (auto *SetF = Mod.getFunction("amd.dx.setLocalRootIndex")) {
    SmallDenseSet<Function *> HasSetF;

    llvm::forEachCall(*SetF, [&](CallInst &CInst) {
      // Returns true if it is a new value
      Function *Func = CInst.getFunction();
      auto Inserted = HasSetF.insert(Func);
      Check(Inserted.second, "Found a function with more than one call to setLocalRootIndex", Func);
    });
  }
}

PreservedAnalyses ContinuationsLintPass::run(Module &Mod, ModuleAnalysisManager &AnalysisManager) {
  ContinuationsLintPassImpl Impl{Mod};
  Impl.run();

  return PreservedAnalyses::all();
}

#undef Check
