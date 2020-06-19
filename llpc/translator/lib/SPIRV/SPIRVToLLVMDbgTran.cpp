//===- LLVMToSPIRVDbgTran.cpp - Converts SPIR-V to LLVM ----------------*- C++ -*-===//
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
#include "SPIRVToLLVMDbgTran.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"

using namespace SPIRV;

SPIRVToLLVMDbgTran::SPIRVToLLVMDbgTran(SPIRVModule *tbm, Module *tm)
    : m_bm(tbm), m_m(tm), m_spDbg(m_bm), m_builder(*m_m) {
  m_enable = m_bm->hasDebugInfo();
}

void SPIRVToLLVMDbgTran::createCompileUnit() {
  if (!m_enable)
    return;
  auto file = m_spDbg.getEntryPointFileStr(ExecutionModelVertex, 0);
  if (file.empty())
    file = "spirv.dbg.cu"; // File name must be non-empty
  std::string baseName;
  std::string path;
  splitFileName(file, baseName, path);
  m_builder.createCompileUnit(dwarf::DW_LANG_C99, m_builder.createFile(baseName, path), "spirv", false, "", 0, "",
                              DICompileUnit::LineTablesOnly);
}

void SPIRVToLLVMDbgTran::addDbgInfoVersion() {
  if (!m_enable)
    return;
  m_m->addModuleFlag(Module::Warning, "Dwarf Version", dwarf::DWARF_VERSION);
  m_m->addModuleFlag(Module::Warning, "Debug Info Version", DEBUG_METADATA_VERSION);
}

DIFile *SPIRVToLLVMDbgTran::getDIFile(const std::string &fileName) {
  return getOrInsert(m_fileMap, fileName, [=]() -> DIFile * {
    std::string baseName;
    std::string path;
    splitFileName(fileName, baseName, path);
    return m_builder.createFile(baseName, path);
  });
}

DISubprogram *SPIRVToLLVMDbgTran::getDISubprogram(SPIRVFunction *sf, Function *f) {
  auto *sp = getOrInsert(m_funcMap, f, [=]() {
    auto df = getDIFile(m_spDbg.getFunctionFileStr(sf));
    auto fn = f->getName();
    auto ln = m_spDbg.getFunctionLineNo(sf);
    auto spFlags = DISubprogram::SPFlagDefinition;
    if (Function::isInternalLinkage(f->getLinkage()))
      spFlags |= DISubprogram::SPFlagLocalToUnit;
    return m_builder.createFunction(df, fn, fn, df, ln,
                                    m_builder.createSubroutineType(m_builder.getOrCreateTypeArray(None)), ln,
                                    DINode::FlagZero, spFlags);
  });
  assert(f->getSubprogram() == sp || f->getSubprogram() == nullptr);
  f->setSubprogram(sp);
  return sp;
}

void SPIRVToLLVMDbgTran::transDbgInfo(SPIRVValue *sv, Value *v) {
  if (!m_enable || !sv->hasLine())
    return;
  if (auto i = dyn_cast<Instruction>(v)) {
    assert(sv->isInst() && "Invalid instruction");
    auto si = static_cast<SPIRVInstruction *>(sv);
    assert(si->getParent() && si->getParent()->getParent() && "Invalid instruction");
    auto line = sv->getLine();
    i->setDebugLoc(DebugLoc::get(line->getLine(), line->getColumn(),
                                 getDISubprogram(si->getParent()->getParent(), i->getParent()->getParent())));
  }
}

void SPIRVToLLVMDbgTran::finalize() {
  if (!m_enable)
    return;
  m_builder.finalize();
}

void SPIRVToLLVMDbgTran::splitFileName(const std::string &fileName, std::string &baseName, std::string &path) {
  auto loc = fileName.find_last_of("/\\");
  if (loc != std::string::npos) {
    baseName = fileName.substr(loc + 1);
    path = fileName.substr(0, loc);
  } else {
    baseName = fileName;
    path = ".";
  }
}
