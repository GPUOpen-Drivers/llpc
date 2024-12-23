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
#include "llvm/ADT/SmallSet.h"
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

  using JumpVecTy = SmallVector<lgc::cps::JumpOp *>;
  using AwaitFuncSetTy = SmallSet<Function *, 1>;
  JumpVecTy AllJumps;
  AwaitFuncSetTy FuncsWithAwaits;
  void collectCallInfo();
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
  collectCallInfo();

  checkJumpTargets();
  checkSetLocalRootIndex();

  dbgs() << MessagesStr.str();
  if (ContLintAbortOnError && !MessagesStr.str().empty())
    report_fatal_error(Twine("Continuations linter found errors, aborting. (enabled by --") +
                           ContLintAbortOnErrorArgName + ")",
                       false);
}

void ContinuationsLintPassImpl::collectCallInfo() {
  struct VisitorState {
    JumpVecTy &Jumps;
    AwaitFuncSetTy &FuncsWithAwaits;
  };

  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitorState>()
          .add<lgc::cps::JumpOp>([](VisitorState &S, lgc::cps::JumpOp &Op) { S.Jumps.push_back(&Op); })
          .add<lgc::cps::AwaitOp>(
              [](VisitorState &S, lgc::cps::AwaitOp &Op) { S.FuncsWithAwaits.insert(Op.getFunction()); })
          .build();

  VisitorState S{AllJumps, FuncsWithAwaits};

  Visitor.visit(S, Mod);
}

// Check that every possible jump candidate has a valid jump target
void ContinuationsLintPassImpl::checkJumpTargets() {
  for (auto *JumpCandidate : AllJumps) {
    Value *JumpTarget = JumpCandidate->getTarget();

    assert(JumpTarget);

    Check(!isa<UndefValue>(JumpTarget), "Jump has undefined jump target", JumpCandidate);
  }
}

// Check that every function has at most one setLocalRootIndex call.
void ContinuationsLintPassImpl::checkSetLocalRootIndex() {
  struct VisitorState {
    const AwaitFuncSetTy &FuncsWithAwaits;
    SmallDenseSet<Function *> HasSetF;
    SmallVector<Function *> InvalidFuncs;
  };

  static const auto Visitor =
      llvm_dialects::VisitorBuilder<VisitorState>()
          .add<lgc::ilcps::SetLocalRootIndexOp>([](VisitorState &S, lgc::ilcps::SetLocalRootIndexOp &Op) {
            // Collect all functions that have more than one call to lgc.ilcps.setLocalRootIndex, but only if these
            // calls do not reside in functions that are not yet split.

            // Returns true if it is a new value
            Function *Func = Op.getFunction();
            // It is allowed to have multiple setLocalRootIndex calls if the call resides in a function that was not yet
            // split.
            if (S.FuncsWithAwaits.contains(Func))
              return;

            auto Inserted = S.HasSetF.insert(Func);
            if (!Inserted.second)
              S.InvalidFuncs.push_back(Func);
          })
          .build();

  VisitorState State{FuncsWithAwaits, {}, {}};

  Visitor.visit(State, Mod);

  for (auto *Func : State.InvalidFuncs)
    checkFailed("Found a function with more than one call to setLocalRootIndex", Func);
}

PreservedAnalyses ContinuationsLintPass::run(Module &Mod, ModuleAnalysisManager &AnalysisManager) {
  ContinuationsLintPassImpl Impl{Mod};
  Impl.run();

  return PreservedAnalyses::all();
}

#undef Check
