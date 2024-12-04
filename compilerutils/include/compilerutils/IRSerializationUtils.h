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

//===- IRSerializationUtils.h - Library for compiler frontends ------------===//
//
// Implements several shared helper functions for dumping IR in various forms
// including to DOT files and LL.
//
//===----------------------------------------------------------------------===//

#ifndef IRSERIALIZATIONUTILS_H
#define IRSERIALIZATIONUTILS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

namespace irserializationutils {

// Returns an MD5 hash of the module LL. This is returned as a string so it can
// be used as part of a filename.
std::string getModuleHashStr(const llvm::Module &m);

// Writes a DOT file with the CFG of the function. The filename is:
// FilenamePrefix.FuncName.Hash.dot where FuncName is determined by demangling
// the DXIL function name, and Hash is given by getModuleHashStr.
// Set cfgOnly = false to include instructions within the BBs.
void writeCFGToDotFile(const llvm::Function &f, llvm::StringRef filenamePrefix = "cfg", bool cfgOnly = true);

// Writes an LL file with the module. The filename is:
// FilenamePrefix.Hash.ll where Hash is given by getModuleHashStr.
void writeModuleToLLFile(const llvm::Module &m, llvm::StringRef filenamePrefix = "module");

} // namespace irserializationutils

#endif
