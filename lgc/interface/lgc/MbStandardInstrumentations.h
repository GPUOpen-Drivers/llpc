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

#pragma once

#include "lgc/ModuleBunch.h"
#include "llvm/Passes/StandardInstrumentations.h"

namespace llvm {

// Copy of PrintIRInstrumentation with edits for ModuleBunch.
/// Instrumentation to print IR before/after passes.
///
/// Needs state to be able to print module after pass that invalidates IR unit
/// (typically Loop or SCC).
class MbPrintIRInstrumentation {
public:
  ~MbPrintIRInstrumentation();

  void registerCallbacks(PassInstrumentationCallbacks &PIC);

private:
  void printBeforePass(StringRef PassID, Any IR);
  void printAfterPass(StringRef PassID, Any IR);
  void printAfterPassInvalidated(StringRef PassID);

  bool shouldPrintBeforePass(StringRef PassID);
  bool shouldPrintAfterPass(StringRef PassID);

  using PrintModuleDesc = std::tuple<Any, std::string, StringRef>;

  void pushModuleDesc(StringRef PassID, Any IR);
  PrintModuleDesc popModuleDesc(StringRef PassID);

  PassInstrumentationCallbacks *PIC;
  /// Stack of Module description, enough to print the module after a given
  /// pass.
  SmallVector<PrintModuleDesc, 2> ModuleDescStack;
};

// Debug logging for transformation and analysis passes.
class MbPrintPassInstrumentation {
  raw_ostream &print();

public:
  MbPrintPassInstrumentation(bool Enabled, PrintPassOptions Opts) : Enabled(Enabled), Opts(Opts) {}
  void registerCallbacks(PassInstrumentationCallbacks &PIC);

private:
  bool Enabled;
  PrintPassOptions Opts;
  int Indent = 0;
};

// Copy of VerifyInstrumentation with edits for ModuleBunch.
class MbVerifyInstrumentation {
  bool DebugLogging;

public:
  MbVerifyInstrumentation(bool DebugLogging) : DebugLogging(DebugLogging) {}
  void registerCallbacks(PassInstrumentationCallbacks &PIC);
};

/// This class provides an interface to register all the standard pass
/// instrumentations and manages their state (if any).
/// Ones that have not yet been adapted for use with a ModuleBunch pass manager (ones without an Mb prefix)
/// may well be broken.
class MbStandardInstrumentations {
  MbPrintIRInstrumentation PrintIR;
  MbPrintPassInstrumentation PrintPass;
  TimePassesHandler TimePasses;
  TimeProfilingPassesHandler TimeProfilingPasses;
  OptNoneInstrumentation OptNone;
  // OptPassGateInstrumentation OptPassGate; // Cannot even attempt to use this as it needs LLVMContext
  PreservedCFGCheckerInstrumentation PreservedCFGChecker;
  IRChangedPrinter PrintChangedIR;
  PseudoProbeVerifier PseudoProbeVerification;
  InLineChangePrinter PrintChangedDiff;
  DotCfgChangeReporter WebsiteChangeReporter;
  PrintCrashIRInstrumentation PrintCrashIR;
  IRChangedTester ChangeTester;
  MbVerifyInstrumentation Verify;

  bool VerifyEach;

public:
  MbStandardInstrumentations(bool DebugLogging, bool VerifyEach = false,
                             PrintPassOptions PrintPassOpts = PrintPassOptions());

  // Register all the standard instrumentation callbacks. If \p FAM is nullptr
  // then PreservedCFGChecker is not enabled.
  void registerCallbacks(PassInstrumentationCallbacks &PIC,
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 454783
                         // Old version of the code
                         FunctionAnalysisManager *FAM = nullptr);
#else
                         // New version of the code (also handles unknown version, which we treat as latest)
                         ModuleAnalysisManager *MAM = nullptr);
#endif

  TimePassesHandler &getTimePasses() { return TimePasses; }
};

} // namespace llvm
