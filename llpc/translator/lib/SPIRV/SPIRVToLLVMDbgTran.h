//===- LLVMToSPIRVDbgTran.h - Converts SPIR-V to LLVM ------------------*- C++ -*-===//
//
//                     The LLVM/SPIR-V Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file implements translation of debug info from SPIR-V to LLVM metadata
///
//===----------------------------------------------------------------------===//
#ifndef SPIRV_LLVMTOSPIRVDBGTRAN_H
#define SPIRV_LLVMTOSPIRVDBGTRAN_H

#include "SPIRVModule.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace SPIRV {

class SPIRVToLLVMDbgTran {
public:
  SPIRVToLLVMDbgTran(SPIRVModule *tbm, Module *tm);

  void createCompileUnit();

  void addDbgInfoVersion();

  DIFile *getDIFile(const std::string &fileName);

  DISubprogram *getDISubprogram(SPIRVFunction *sf, Function *f);

  void transDbgInfo(SPIRVValue *sv, Value *v);

  void finalize();

private:
  SPIRVModule *m_bm;
  Module *m_m;
  SPIRVDbgInfo m_spDbg;
  DIBuilder m_builder;
  bool m_enable;
  std::unordered_map<std::string, DIFile *> m_fileMap;
  std::unordered_map<Function *, DISubprogram *> m_funcMap;

  void splitFileName(const std::string &fileName, std::string &baseName, std::string &path);
};

} // namespace SPIRV

#endif // SPIRV_LLVMTOSPIRVDBGTRAN_H
