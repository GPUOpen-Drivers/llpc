//===- SPIRVStream.cpp - Class to represent a SPIR-V Stream ------*- C++ -*-===//
//
//                     The LLVM/SPIRV Translator
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
/// This file implements SPIR-V stream class.
///
//===----------------------------------------------------------------------===//
#include "SPIRVStream.h"
#include "SPIRVDebug.h"
#include "SPIRVFunction.h"
#include "SPIRVNameMapEnum.h"
#include "SPIRVOpCode.h"

namespace SPIRV {

SPIRVDecoder::SPIRVDecoder(std::istream &InputStream, SPIRVFunction &F)
    : IS(InputStream), M(*F.getModule()), WordCount(0), OpCode(OpNop),
      Scope(&F) {}

SPIRVDecoder::SPIRVDecoder(std::istream &InputStream, SPIRVBasicBlock &BB)
    : IS(InputStream), M(*BB.getModule()), WordCount(0), OpCode(OpNop),
      Scope(&BB) {}

void SPIRVDecoder::setScope(SPIRVEntry *TheScope) {
  assert(TheScope && (TheScope->getOpCode() == OpFunction ||
                      TheScope->getOpCode() == OpLabel));
  Scope = TheScope;
}

template <class T> const SPIRVDecoder &decode(const SPIRVDecoder &I, T &V) {
  return decodeBinary(I, V);
}

#define SPIRV_DEF_ENCDEC(Type)                                                 \
  const SPIRVDecoder &operator>>(const SPIRVDecoder &I, Type &V) {             \
    return decode(I, V);                                                       \
  }                                                                            \

SPIRV_DEF_ENCDEC(Op)
SPIRV_DEF_ENCDEC(Capability)
SPIRV_DEF_ENCDEC(Decoration)
SPIRV_DEF_ENCDEC(GLSLExtOpKind)
SPIRV_DEF_ENCDEC(SPIRVDebugExtOpKind)
SPIRV_DEF_ENCDEC(LinkageType)

// Read a string with padded 0's at the end so that they form a stream of
// words.
const SPIRVDecoder &operator>>(const SPIRVDecoder &I, std::string &Str) {
  uint64_t Count = 0;
  char Ch;
  while (I.IS.get(Ch) && Ch != '\0') {
    Str += Ch;
    ++Count;
  }
  Count = (Count + 1) % 4;
  Count = Count ? 4 - Count : 0;
  for (; Count; --Count) {
    I.IS >> Ch;
    assert(Ch == '\0' && "Invalid string in SPIRV");
  }
  return I;
}

bool SPIRVDecoder::getWordCountAndOpCode() {
  if (IS.eof()) {
    WordCount = 0;
    OpCode = OpNop;
    return false;
  }
  SPIRVWord WordCountAndOpCode;
  *this >> WordCountAndOpCode;
  WordCount = WordCountAndOpCode >> 16;
  OpCode = static_cast<Op>(WordCountAndOpCode & 0xFFFF);
  assert(!IS.bad() && "SPIRV stream is bad");
  if (IS.fail()) {
    WordCount = 0;
    OpCode = OpNop;
    return false;
  }
  return true;
}

SPIRVEntry *SPIRVDecoder::getEntry() {
  if (WordCount == 0 || OpCode == OpNop)
    return nullptr;
  SPIRVEntry *Entry = SPIRVEntry::create(OpCode);
  assert(Entry);
  Entry->setModule(&M);
  if (!Scope && (isModuleScopeAllowedOpCode(OpCode) || OpCode == OpExtInst)) {
  } else
    Entry->setScope(Scope);
  Entry->setWordCount(WordCount);
  Entry->setLine(M.getCurrentLine());
  IS >> *Entry;
  if(Entry->isEndOfBlock() || OpCode == OpNoLine)
    M.setCurrentLine(nullptr);
  assert(!IS.bad() && !IS.fail() && "SPIRV stream fails");
  M.add(Entry);
  return Entry;
}

void SPIRVDecoder::validate() const {
  assert(OpCode != OpNop && "Invalid op code");
  assert(WordCount && "Invalid word count");
  assert(!IS.bad() && "Bad iInput stream");
}

} // namespace SPIRV
