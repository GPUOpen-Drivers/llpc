/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  cross-module-inline.cpp
 * @brief Command-line utility that allows to test cross-module inlining.
 ***********************************************************************************************************************
 */

#include "compilerutils/CompilerUtils.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cstdlib>

using namespace llvm;

namespace {
// Input file for the module that is inlined to ("-" for stdin
cl::opt<std::string> MainModule(cl::Positional, cl::ValueRequired, cl::desc("main_module"));
// Input file for the module that is inlined from ("-" for stdin)
cl::opt<std::string> LinkModule(cl::Positional, cl::ValueRequired, cl::desc("link_module"));

cl::list<std::string>
    LinkFunction("link", cl::desc("Name of the function to link and inline from the link_module to the main_module"));

cl::opt<std::string> OutFileName("o", cl::desc("Output filename ('-' for stdout)"), cl::value_desc("filename"));

std::unique_ptr<Module> parseIr(LLVMContext &context, std::string &filename) {
  llvm::SMDiagnostic error;

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> inputFileOrErr =
      llvm::MemoryBuffer::getFileOrSTDIN(filename, /*IsText=*/false);
  if (std::error_code errorCode = inputFileOrErr.getError()) {
    auto error = SMDiagnostic(filename, SourceMgr::DK_Error,
                              "Could not open input file '" + filename + "': " + errorCode.message());
    error.print("cross-module-inline", errs());
    errs() << "\n";
    exit(EXIT_FAILURE);
  }
  auto inputFileBuffer = std::move(inputFileOrErr.get());

  // Parse as IR file
  auto mod = llvm::parseIR(inputFileBuffer->getMemBufferRef(), error, context);
  if (!mod) {
    error.print("cross-module-inline", errs());
    errs() << "\n";
    exit(EXIT_FAILURE);
  }
  return mod;
}
} // anonymous namespace

// =====================================================================================================================
// Main code of the testing utility
//
// @param argc : Count of command-line arguments
// @param argv : Command-line arguments
int main(int argc, char **argv) {
  const char *progName = sys::path::filename(argv[0]).data();

  // Parse command line.
  static const char *commandDesc = "cross-module-inline: inline from one module into another\n";
  cl::ParseCommandLineOptions(argc, argv, commandDesc);

  // Read input files.
  llvm::SMDiagnostic error;
  llvm::LLVMContext context;

  auto mainMod = parseIr(context, MainModule);
  auto linkMod = parseIr(context, LinkModule);

  CompilerUtils::CrossModuleInliner inliner;
  for (auto &linkName : LinkFunction) {
    // Search for calls and inline them
    auto *linkF = mainMod->getFunction(linkName);
    if (!linkF)
      report_fatal_error(Twine("Function '") + linkName + "' not found in main module");

    auto *targetF = linkMod->getFunction(linkName);
    if (!targetF)
      report_fatal_error(Twine("Function '") + linkName + "' not found in link module");

    for (auto &Use : make_early_inc_range(linkF->uses())) {
      if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
        if (CInst->isCallee(&Use)) {
          // Change call target to other module
          Use = targetF;

          inliner.inlineCall(*CInst);
        }
      }
    }
  }

  // Output
  if (OutFileName.getNumOccurrences() && (OutFileName != "") && (OutFileName != "-")) {
    // Write to file
    std::error_code errorCode;
    llvm::raw_fd_ostream file(OutFileName, errorCode, llvm::sys::fs::OF_Text);
    mainMod->print(file, nullptr);
    file.close();
    if (errorCode) {
      auto error = SMDiagnostic(OutFileName, SourceMgr::DK_Error, "Could not open output file: " + errorCode.message());
      error.print(progName, errs());
      errs() << "\n";
      return EXIT_FAILURE;
    }
  } else {
    // Print to stdout
    mainMod->print(llvm::outs(), nullptr);
  }

  return EXIT_SUCCESS;
}
