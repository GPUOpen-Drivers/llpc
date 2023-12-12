//===- SPIRVDecorate.cpp -SPIR-V Decorations ---------------------*- C++ -*-===//
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
/// This file implements SPIR-V decorations.
///
//===----------------------------------------------------------------------===//

#include "SPIRVDecorate.h"
#include "SPIRVModule.h"
#include "SPIRVStream.h"
#include "SPIRVValue.h"

namespace SPIRV {

SPIRVDecorateGeneric::SPIRVDecorateGeneric(Op OC, SPIRVWord WC, Decoration TheDec, SPIRVEntry *TheTarget)
    : SPIRVAnnotationGeneric(TheTarget->getModule(), WC, OC, TheTarget->getId()), Dec(TheDec), Owner(nullptr) {
  validate();
  updateModuleVersion();
}

SPIRVDecorateGeneric::SPIRVDecorateGeneric(Op OC, SPIRVWord WC, Decoration TheDec, SPIRVEntry *TheTarget, SPIRVWord V)
    : SPIRVAnnotationGeneric(TheTarget->getModule(), WC, OC, TheTarget->getId()), Dec(TheDec), Owner(nullptr) {
  Literals.push_back(V);
  validate();
  updateModuleVersion();
}

SPIRVDecorateGeneric::SPIRVDecorateGeneric(Op OC)
    : SPIRVAnnotationGeneric(OC), Dec(DecorationRelaxedPrecision), Owner(nullptr) {
}

Decoration SPIRVDecorateGeneric::getDecorateKind() const {
  return Dec;
}

SPIRVWord SPIRVDecorateGeneric::getLiteral(size_t I) const {
  assert(I <= Literals.size() && "Out of bounds");
  return Literals[I];
}

SPIRVEntry *SPIRVDecorateGeneric::getEntry(size_t I) const {
  assert(I <= Ids.size() && "Out of bounds");
  return Module->getEntry(Ids[I]);
}

size_t SPIRVDecorateGeneric::getLiteralCount() const {
  return Literals.size();
}

void SPIRVDecorate::setWordCount(SPIRVWord Count) {
  WordCount = Count;
  Literals.resize(WordCount - FixedWC);
}

void SPIRVDecorate::decode(std::istream &I) {
  SPIRVDecoder Decoder = getDecoder(I);
  Decoder >> Target >> Dec;
  if (Dec == DecorationLinkageAttributes)
    SPIRVDecorateLinkageAttr::decodeLiterals(Decoder, Literals);
  else
    Decoder >> Literals;
  getOrCreateTarget()->addDecorate(this);
}

void SPIRVMemberDecorate::setWordCount(SPIRVWord Count) {
  WordCount = Count;
  Literals.resize(WordCount - FixedWC);
}

void SPIRVMemberDecorate::decode(std::istream &I) {
  getDecoder(I) >> Target >> MemberNumber >> Dec >> Literals;
  getOrCreateTarget()->addMemberDecorate(this);
}

void SPIRVDecorationGroup::decode(std::istream &I) {
  getDecoder(I) >> Id;
  Module->addDecorationGroup(this);
}

void SPIRVGroupDecorate::decode(std::istream &I) {
  getDecoder(I) >> DecorationGroup >> Targets;
  Module->addGroupDecorateGeneric(this);
}

void SPIRVGroupDecorate::decorateTargets() {
  for (auto &I : Targets) {
    auto Target = getOrCreate(I);
    for (auto &Dec : DecorationGroup->getDecorations()) {
      assert(Dec->isDecorate());
      Target->addDecorate(static_cast<SPIRVDecorate *const>(Dec));
    }
  }
}

void SPIRVGroupMemberDecorate::decode(std::istream &I) {
  std::vector<SPIRVWord> Pairs(WordCount - FixedWC);
  getDecoder(I) >> DecorationGroup >> Pairs;
  assert(Pairs.size() % 2 == 0);
  for (uint32_t J = 0, E = Pairs.size(); J < E; J += 2) {
    Targets.push_back(Pairs[J]);
    MemberNumbers.push_back(Pairs[J + 1]);
  }
  Module->addGroupDecorateGeneric(this);
}

void SPIRVGroupMemberDecorate::decorateTargets() {
  assert(Targets.size() == MemberNumbers.size());
  for (uint32_t I = 0, E = Targets.size(); I < E; ++I) {
    auto Target = getOrCreate(Targets[I]);
    for (auto &Dec : DecorationGroup->getDecorations()) {
      assert(Dec->isDecorate());
      auto TheDec = static_cast<const SPIRVDecorate *>(Dec);
      if (TheDec->getLiteralCount() == 0)
        Target->addMemberDecorate(MemberNumbers[I], TheDec->getDecorateKind());
      else {
        assert(TheDec->getLiteralCount() == 1);
        Target->addMemberDecorate(MemberNumbers[I], TheDec->getDecorateKind(), TheDec->getLiteral(0));
      }
    }
  }
}

void SPIRVDecorateId::setWordCount(SPIRVWord Count) {
  WordCount = Count;
  Ids.resize(WordCount - FixedWC);
}

void SPIRVDecorateId::decode(std::istream &I) {
  SPIRVDecoder Decoder = getDecoder(I);
  Decoder >> Target >> Dec;
  Decoder >> Ids;
  getOrCreateTarget()->addDecorate(this);
}

} // namespace SPIRV
