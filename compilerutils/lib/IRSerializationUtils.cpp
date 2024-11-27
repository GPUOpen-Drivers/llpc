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
#include "compilerutils/IRSerializationUtils.h"
#include "compilerutils/DxilUtils.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/MD5.h"
#include <iomanip>
#include <sstream>

using namespace llvm;
using namespace irserializationutils;

// Returns an MD5 hash of the module LL. This is returned as a string so it can
// be used as part of a filename.
std::string irserializationutils::getModuleHashStr(const Module &m) {
  std::string moduleStr;
  raw_string_ostream os(moduleStr);
  os << m;

  MD5 hash;
  MD5::MD5Result result;
  hash.update(moduleStr);
  hash.final(result);

  SmallString<32> resStr;
  MD5::stringifyResult(result, resStr);
  std::stringstream hexStream;
  for (char value : resStr)
    hexStream << std::hex << value;

  return hexStream.str();
}

static void writeToHashedOutputFile(const Module &m, StringRef filenamePrefix, StringRef filenameExt,
                                    std::function<void(raw_fd_ostream &)> callback) {
  // LLVM_DEBUG is not used in this function because the call will already be
  // guarded by a DEBUG macro, such as: DEBUG_WITH_TYPE(...);

  auto hash = getModuleHashStr(m);
  auto fullName = filenamePrefix + "." + hash + "." + filenameExt;

  // If a file with an identical hash exists then we don't need to write it
  // again.
  if (sys::fs::exists(fullName))
    return;

  std::error_code ec;
  raw_fd_ostream file(fullName.str(), ec, sys::fs::OF_Text);

  if (ec) {
    errs() << "Error opening " << fullName << " : " << ec.message() << "\n";
    return;
  }

  callback(file);

  dbgs() << "Wrote file '" << fullName << "'\n";
}

void irserializationutils::writeCFGToDotFile(const Function &f, StringRef filenamePrefix, bool cfgOnly) {
  // LLVM_DEBUG is not used in this function because the call will already be
  // guarded by a DEBUG macro, such as: DEBUG_WITH_TYPE(...);
  auto funcName = CompilerUtils::dxil::tryDemangleFunctionName(f.getName());
  auto filenamePrefixFuncName = filenamePrefix.str() + "." + funcName.str();

  writeToHashedOutputFile(*f.getParent(), filenamePrefixFuncName, "dot", [&](raw_fd_ostream &file) {
    DOTFuncInfo cfgInfo(&f);
    WriteGraph(file, &cfgInfo, cfgOnly);
  });
}

void irserializationutils::writeModuleToLLFile(const Module &m, StringRef filenamePrefix) {
  writeToHashedOutputFile(m, filenamePrefix, "ll", [&](raw_fd_ostream &file) { file << m; });
}
