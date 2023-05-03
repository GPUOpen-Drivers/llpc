/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

// An alternative to LLVM's StandardInstrumentations that (partly) patches
// things up so they work on ModuleBunch passes.
// Most code here is copied from LLVM's StandardInstrumentations.cpp and
// edited.

#include "lgc/MbStandardInstrumentations.h"
#include "llvm/IR/PrintPasses.h"
#include "llvm/IR/Verifier.h"

using namespace llvm;

namespace {

/// Extract the outermost IR unit out of \p IR unit. May return wrapped nullptr if \p IR does not match
/// certain global filters. Will never return wrapped nullptr if \p Force is true.
Any unwrapOuter(Any IR, bool Force = false) {
  if (const auto **MB = any_cast<const ModuleBunch *>(&IR))
    return *MB;
  if (const auto **M = any_cast<const Module *>(&IR))
    return *M;

  if (const auto **F = any_cast<const Function *>(&IR)) {
    if (!Force && !isFunctionInPrintList((*F)->getName()))
      return nullptr;

    return (*F)->getParent();
  }

  if (const auto **C = any_cast<const LazyCallGraph::SCC *>(&IR)) {
    for (const LazyCallGraph::Node &N : **C) {
      const Function &F = N.getFunction();
      if (Force || (!F.isDeclaration() && isFunctionInPrintList(F.getName()))) {
        return F.getParent();
      }
    }
    assert(!Force && "Expected a module");
    return nullptr;
  }

  if (const auto **L = any_cast<const Loop *>(&IR)) {
    const Function *F = (*L)->getHeader()->getParent();
    if (!Force && !isFunctionInPrintList(F->getName()))
      return nullptr;
    return F->getParent();
  }

  llvm_unreachable("Unknown IR unit");
}

void printIR(raw_ostream &OS, const Function *F) {
  if (!isFunctionInPrintList(F->getName()))
    return;
  OS << *F;
}

void printIR(raw_ostream &OS, const Module *M) {
  if (isFunctionInPrintList("*") || forcePrintModuleIR()) {
    M->print(OS, nullptr);
  } else {
    for (const auto &F : M->functions()) {
      printIR(OS, &F);
    }
  }
}

void printIR(raw_ostream &OS, const ModuleBunch *MB) {
  for (Module &M : *MB)
    printIR(OS, &M);
}

void printIR(raw_ostream &OS, const LazyCallGraph::SCC *C) {
  for (const LazyCallGraph::Node &N : *C) {
    const Function &F = N.getFunction();
    if (!F.isDeclaration() && isFunctionInPrintList(F.getName())) {
      F.print(OS);
    }
  }
}

void printIR(raw_ostream &OS, const Loop *L) {
  const Function *F = L->getHeader()->getParent();
  if (!isFunctionInPrintList(F->getName()))
    return;
  printLoop(const_cast<Loop &>(*L), OS);
}

std::string getIRName(Any IR) {
  if (any_cast<const ModuleBunch *>(&IR))
    return "[moduleBunch]";

  if (any_cast<const Module *>(&IR))
    return "[module]";

  if (const auto **F = any_cast<const Function *>(&IR))
    return (*F)->getName().str();

  if (const auto **C = any_cast<const LazyCallGraph::SCC *>(&IR))
    return (*C)->getName();

  if (const auto **L = any_cast<const Loop *>(&IR))
    return (*L)->getName().str();

  llvm_unreachable("Unknown wrapped IR type");
}

bool moduleContainsFilterPrintFunc(const Module &M) {
  return any_of(M.functions(), [](const Function &F) { return isFunctionInPrintList(F.getName()); }) ||
         isFunctionInPrintList("*");
}

bool sccContainsFilterPrintFunc(const LazyCallGraph::SCC &C) {
  return any_of(C, [](const LazyCallGraph::Node &N) { return isFunctionInPrintList(N.getName()); }) ||
         isFunctionInPrintList("*");
}

bool shouldPrintIR(Any IR) {
  if (const auto **MB = any_cast<const ModuleBunch *>(&IR)) {
    bool ShouldPrint = false;
    for (Module &M : **MB)
      ShouldPrint |= moduleContainsFilterPrintFunc(M);
    return ShouldPrint;
  }

  if (const auto **M = any_cast<const Module *>(&IR))
    return moduleContainsFilterPrintFunc(**M);

  if (const auto **F = any_cast<const Function *>(&IR))
    return isFunctionInPrintList((*F)->getName());

  if (const auto **C = any_cast<const LazyCallGraph::SCC *>(&IR))
    return sccContainsFilterPrintFunc(**C);

  if (const auto **L = any_cast<const Loop *>(&IR))
    return isFunctionInPrintList((*L)->getHeader()->getParent()->getName());
  llvm_unreachable("Unknown wrapped IR type");
}

/// Generic IR-printing helper that unpacks a pointer to IRUnit wrapped into
/// Any and does actual print job.
void unwrapAndPrint(raw_ostream &OS, Any IR) {
  if (!shouldPrintIR(IR))
    return;

  if (forcePrintModuleIR())
    IR = unwrapOuter(IR);

  if (const auto **MB = any_cast<const ModuleBunch *>(&IR)) {
    printIR(OS, *MB);
    return;
  }

  if (const auto **M = any_cast<const Module *>(&IR)) {
    printIR(OS, *M);
    return;
  }

  if (const auto **F = any_cast<const Function *>(&IR)) {
    printIR(OS, *F);
    return;
  }

  if (const auto **C = any_cast<const LazyCallGraph::SCC *>(&IR)) {
    printIR(OS, *C);
    return;
  }

  if (const auto **L = any_cast<const Loop *>(&IR)) {
    printIR(OS, *L);
    return;
  }
  llvm_unreachable("Unknown wrapped IR type");
}

// Return true when this is a pass for which changes should be ignored
bool isIgnored(StringRef PassID) {
  return isSpecialPass(PassID, {"PassManager", "PassAdaptor", "AnalysisManagerProxy", "DevirtSCCRepeatedPass",
                                "ModuleInlinerWrapperPass"});
}

} // anonymous namespace

MbPrintIRInstrumentation::~MbPrintIRInstrumentation() {
  assert(ModuleDescStack.empty() && "ModuleDescStack is not empty at exit");
}

void MbPrintIRInstrumentation::pushModuleDesc(StringRef PassID, Any IR) {
  ModuleDescStack.emplace_back(unwrapOuter(IR), getIRName(IR), PassID);
}

MbPrintIRInstrumentation::PrintModuleDesc MbPrintIRInstrumentation::popModuleDesc(StringRef PassID) {
  assert(!ModuleDescStack.empty() && "empty ModuleDescStack");
  PrintModuleDesc ModuleDesc = ModuleDescStack.pop_back_val();
  assert(std::get<2>(ModuleDesc).equals(PassID) && "malformed ModuleDescStack");
  return ModuleDesc;
}

void MbPrintIRInstrumentation::printBeforePass(StringRef PassID, Any IR) {
  if (isIgnored(PassID))
    return;

  // Saving Module for AfterPassInvalidated operations.
  // Note: here we rely on a fact that we do not change modules while
  // traversing the pipeline, so the latest captured module is good
  // for all print operations that has not happen yet.
  if (shouldPrintAfterPass(PassID))
    pushModuleDesc(PassID, IR);

  if (!shouldPrintBeforePass(PassID))
    return;

  if (!shouldPrintIR(IR))
    return;

  dbgs() << "*** IR Dump Before " << PassID << " on " << getIRName(IR) << " ***\n";
  unwrapAndPrint(dbgs(), IR);
}

void MbPrintIRInstrumentation::printAfterPass(StringRef PassID, Any IR) {
  if (isIgnored(PassID))
    return;

  if (!shouldPrintAfterPass(PassID))
    return;

  Any OuterIR;
  std::string IRName;
  StringRef StoredPassID;
  std::tie(OuterIR, IRName, StoredPassID) = popModuleDesc(PassID);
  assert(StoredPassID == PassID && "mismatched PassID");

  if (!shouldPrintIR(IR))
    return;

  dbgs() << "*** IR Dump After " << PassID << " on " << IRName << " ***\n";
  unwrapAndPrint(dbgs(), IR);
}

void MbPrintIRInstrumentation::printAfterPassInvalidated(StringRef PassID) {
  StringRef PassName = PIC->getPassNameForClassName(PassID);
  if (!shouldPrintAfterPass(PassName))
    return;

  if (isIgnored(PassID))
    return;

  Any OuterIR;
  std::string IRName;
  StringRef StoredPassID;
  std::tie(OuterIR, IRName, StoredPassID) = popModuleDesc(PassID);
  assert(StoredPassID == PassID && "mismatched PassID");
  // Additional filtering (e.g. -filter-print-func) can lead to module
  // printing being skipped.
  if (!*any_cast<const void *>(&OuterIR))
    return;

  SmallString<20> Banner = formatv("*** IR Dump After {0} on {1} (invalidated) ***", PassID, IRName);
  dbgs() << Banner << "\n";
  unwrapAndPrint(dbgs(), OuterIR);
}

bool MbPrintIRInstrumentation::shouldPrintBeforePass(StringRef PassID) {
  if (shouldPrintBeforeAll())
    return true;

  StringRef PassName = PIC->getPassNameForClassName(PassID);
  return is_contained(printBeforePasses(), PassName);
}

bool MbPrintIRInstrumentation::shouldPrintAfterPass(StringRef PassID) {
  if (shouldPrintAfterAll())
    return true;

  StringRef PassName = PIC->getPassNameForClassName(PassID);
  return is_contained(printAfterPasses(), PassName);
}

void MbPrintIRInstrumentation::registerCallbacks(PassInstrumentationCallbacks &PIC) {
  this->PIC = &PIC;

  // BeforePass callback is not just for printing, it also saves a Module
  // for later use in AfterPassInvalidated.
  if (shouldPrintBeforeSomePass() || shouldPrintAfterSomePass())
    PIC.registerBeforeNonSkippedPassCallback([this](StringRef P, Any IR) { this->printBeforePass(P, IR); });

  if (shouldPrintAfterSomePass()) {
    PIC.registerAfterPassCallback(
        [this](StringRef P, Any IR, const PreservedAnalyses &) { this->printAfterPass(P, IR); });
    PIC.registerAfterPassInvalidatedCallback(
        [this](StringRef P, const PreservedAnalyses &) { this->printAfterPassInvalidated(P); });
  }
}

void MbVerifyInstrumentation::registerCallbacks(PassInstrumentationCallbacks &PIC) {
  PIC.registerAfterPassCallback([this](StringRef P, Any IR, const PreservedAnalyses &PassPA) {
    if (isIgnored(P) || P == "VerifierPass")
      return;
    const Function **FPtr = any_cast<const Function *>(&IR);
    const Function *F = FPtr ? *FPtr : nullptr;
    if (!F) {
      if (const auto **L = any_cast<const Loop *>(&IR))
        F = (*L)->getHeader()->getParent();
    }

    if (F) {
      if (DebugLogging)
        dbgs() << "Verifying function " << F->getName() << "\n";

      if (verifyFunction(*F, &errs()))
        report_fatal_error("Broken function found, compilation aborted!");
    } else if (const ModuleBunch **MB = any_cast<const ModuleBunch *>(&IR)) {
      for (Module &M : **MB) {
        if (DebugLogging)
          dbgs() << "Verifying module " << M.getName() << "\n";

        if (verifyModule(M, &errs()))
          report_fatal_error("Broken module found, compilation aborted!");
      }
    } else {
      const Module **MPtr = any_cast<const Module *>(&IR);
      const Module *M = MPtr ? *MPtr : nullptr;
      if (!M) {
        if (const auto **C = any_cast<const LazyCallGraph::SCC *>(&IR))
          M = (*C)->begin()->getFunction().getParent();
      }

      if (M) {
        if (DebugLogging)
          dbgs() << "Verifying module " << M->getName() << "\n";

        if (verifyModule(*M, &errs()))
          report_fatal_error("Broken module found, compilation aborted!");
      }
    }
  });
}

raw_ostream &MbPrintPassInstrumentation::print() {
  if (Opts.Indent) {
    assert(Indent >= 0);
    dbgs().indent(Indent);
  }
  return dbgs();
}

void MbPrintPassInstrumentation::registerCallbacks(PassInstrumentationCallbacks &PIC) {
  if (!Enabled)
    return;

  std::vector<StringRef> SpecialPasses;
  if (!Opts.Verbose) {
    SpecialPasses.emplace_back("PassManager");
    SpecialPasses.emplace_back("PassAdaptor");
  }

  PIC.registerBeforeSkippedPassCallback([this, SpecialPasses](StringRef PassID, Any IR) {
    assert(!isSpecialPass(PassID, SpecialPasses) && "Unexpectedly skipping special pass");

    print() << "Skipping pass: " << PassID << " on " << getIRName(IR) << "\n";
  });
  PIC.registerBeforeNonSkippedPassCallback([this, SpecialPasses](StringRef PassID, Any IR) {
    if (isSpecialPass(PassID, SpecialPasses))
      return;

    auto &OS = print();
    OS << "Running pass: " << PassID << " on " << getIRName(IR);
    if (const auto **F = any_cast<const Function *>(&IR)) {
      unsigned Count = (*F)->getInstructionCount();
      OS << " (" << Count << " instruction";
      if (Count != 1)
        OS << 's';
      OS << ')';
    } else if (const auto **C = any_cast<const LazyCallGraph::SCC *>(&IR)) {
      int Count = (*C)->size();
      OS << " (" << Count << " node";
      if (Count != 1)
        OS << 's';
      OS << ')';
    }
    OS << "\n";
    Indent += 2;
  });
  PIC.registerAfterPassCallback([this, SpecialPasses](StringRef PassID, Any IR, const PreservedAnalyses &) {
    if (isSpecialPass(PassID, SpecialPasses))
      return;

    Indent -= 2;
  });
  PIC.registerAfterPassInvalidatedCallback([this, SpecialPasses](StringRef PassID, Any IR) {
    if (isSpecialPass(PassID, SpecialPasses))
      return;

    Indent -= 2;
  });

  if (!Opts.SkipAnalyses) {
    PIC.registerBeforeAnalysisCallback([this](StringRef PassID, Any IR) {
      print() << "Running analysis: " << PassID << " on " << getIRName(IR) << "\n";
      Indent += 2;
    });
    PIC.registerAfterAnalysisCallback([this](StringRef PassID, Any IR) { Indent -= 2; });
    PIC.registerAnalysisInvalidatedCallback([this](StringRef PassID, Any IR) {
      print() << "Invalidating analysis: " << PassID << " on " << getIRName(IR) << "\n";
    });
    PIC.registerAnalysesClearedCallback(
        [this](StringRef IRName) { print() << "Clearing all analysis results for: " << IRName << "\n"; });
  }
}

MbStandardInstrumentations::MbStandardInstrumentations(bool DebugLogging, bool VerifyEach,
                                                       PrintPassOptions PrintPassOpts)
    : PrintPass(DebugLogging, PrintPassOpts), OptNone(DebugLogging),
      PrintChangedIR(PrintChanged == ChangePrinter::Verbose),
      PrintChangedDiff(PrintChanged == ChangePrinter::DiffVerbose || PrintChanged == ChangePrinter::ColourDiffVerbose,
                       PrintChanged == ChangePrinter::ColourDiffVerbose ||
                           PrintChanged == ChangePrinter::ColourDiffQuiet),
      WebsiteChangeReporter(PrintChanged == ChangePrinter::DotCfgVerbose), Verify(DebugLogging),
      VerifyEach(VerifyEach) {
}

// Copied from LLVM's StandardInstrumentations.cpp and edited.
void MbStandardInstrumentations::registerCallbacks(
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 454783
    // Old version of the code
    PassInstrumentationCallbacks &PIC, FunctionAnalysisManager *FAM) {
#else
    // New version of the code (also handles unknown version, which we treat as latest)
    PassInstrumentationCallbacks &PIC, ModuleAnalysisManager *MAM) {
#endif
  PrintIR.registerCallbacks(PIC);
  PrintPass.registerCallbacks(PIC);
  TimePasses.registerCallbacks(PIC);
  OptNone.registerCallbacks(PIC);
  // OptPassGate.registerCallbacks(PIC);
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 454783
  // Old version of the code
  if (FAM)
    PreservedCFGChecker.registerCallbacks(PIC, *FAM);
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  if (MAM)
    PreservedCFGChecker.registerCallbacks(PIC, *MAM);
#endif
  PrintChangedIR.registerCallbacks(PIC);
  PseudoProbeVerification.registerCallbacks(PIC);
  if (VerifyEach)
    Verify.registerCallbacks(PIC);
  PrintChangedDiff.registerCallbacks(PIC);
  WebsiteChangeReporter.registerCallbacks(PIC);

  ChangeTester.registerCallbacks(PIC);

  PrintCrashIR.registerCallbacks(PIC);
  // TimeProfiling records the pass running time cost.
  // Its 'BeforePassCallback' can be appended at the tail of all the
  // BeforeCallbacks by calling `registerCallbacks` in the end.
  // Its 'AfterPassCallback' is put at the front of all the
  // AfterCallbacks by its `registerCallbacks`. This is necessary
  // to ensure that other callbacks are not included in the timings.
  TimeProfilingPasses.registerCallbacks(PIC);
}
