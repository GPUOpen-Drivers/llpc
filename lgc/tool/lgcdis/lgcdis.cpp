/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  lgcdis.cpp
 * @brief LGC's disassembler command-line utility
 ***********************************************************************************************************************
 */

#include "lgc/Disassembler.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"

using namespace lgc;
using namespace llvm;

namespace {
// Category for lgc options that are shown in "-help".
cl::OptionCategory LgcDisCategory("lgcdis");

// Input sources
cl::list<std::string> InFiles(cl::Positional, cl::OneOrMore, cl::ValueRequired, cl::cat(LgcDisCategory),
                              cl::desc("Input file(s) (\"-\" for stdin)"));

// -o: output filename
cl::opt<std::string> OutFileName("o", cl::cat(LgcDisCategory), cl::desc("Output filename ('-' for stdout)"),
                                 cl::value_desc("filename"));
} // anonymous namespace

// =====================================================================================================================
// Main code of LGC disassembler utility
//
// @param argc : Count of command-line arguments
// @param argv : Command-line arguments
int main(int argc, char **argv) {
  const char *progName = sys::path::filename(argv[0]).data();

  // Set our category on options that we want to show in -help, and hide other options.
  auto opts = cl::getRegisteredOptions();
  cl::HideUnrelatedOptions(LgcDisCategory);

  // Parse command line.
  static const char *commandDesc = "lgcdis: disassemble object file\n";
  cl::ParseCommandLineOptions(argc, argv, commandDesc);

  // Open output file.
  std::error_code errorCode;
  raw_fd_ostream ostream(OutFileName.empty() ? "-" : StringRef(OutFileName), errorCode);
  if (errorCode) {
    auto error = SMDiagnostic(OutFileName, SourceMgr::DK_Error, "Could not open output file: " + errorCode.message());
    error.print(progName, errs());
    errs() << "\n";
    return 1;
  }

  // Read and process each input file.
  SmallVector<std::unique_ptr<MemoryBuffer>, 4> inBuffers;
  for (const auto &inFileName : InFiles) {
    // Read the input file. getFileOrSTDIN handles the case of inFileName being "-".
    ErrorOr<std::unique_ptr<MemoryBuffer>> fileOrErr = MemoryBuffer::getFileOrSTDIN(inFileName);
    if (std::error_code errorCode = fileOrErr.getError()) {
      auto error = SMDiagnostic(inFileName, SourceMgr::DK_Error, "Could not open input file: " + errorCode.message());
      error.print(progName, errs());
      errs() << "\n";
      return 1;
    }
    disassembleObject((*fileOrErr)->getMemBufferRef(), ostream);
  }

  return 0;
}
