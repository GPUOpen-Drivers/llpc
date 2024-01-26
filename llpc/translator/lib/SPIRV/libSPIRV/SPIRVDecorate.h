//===- SPIRVDecorate.h - SPIR-V Decorations ----------------------*- C++ -*-===//
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
/// This file defines SPIR-V decorations.
///
//===----------------------------------------------------------------------===//

#ifndef SPIRV_LIBSPIRV_SPIRVDECORATE_H
#define SPIRV_LIBSPIRV_SPIRVDECORATE_H

#include "SPIRVEntry.h"
#include "SPIRVStream.h"
#include "SPIRVUtil.h"
#include <string>
#include <utility>
#include <vector>

namespace SPIRV {
class SPIRVDecorationGroup;
class SPIRVDecorateGeneric : public SPIRVAnnotationGeneric {
public:
  // Complete constructor for decorations without literals
  SPIRVDecorateGeneric(Op OC, SPIRVWord WC, Decoration TheDec, SPIRVEntry *TheTarget);
  // Complete constructor for decorations with one word literal
  SPIRVDecorateGeneric(Op OC, SPIRVWord WC, Decoration TheDec, SPIRVEntry *TheTarget, SPIRVWord V);
  // Incomplete constructor
  SPIRVDecorateGeneric(Op OC);

  SPIRVWord getLiteral(size_t) const;
  SPIRVEntry *getEntry(size_t) const;
  const char *getLiteralString() const {
    assert(!Literals.empty());
    return reinterpret_cast<const char *>(Literals.data());
  }
  Decoration getDecorateKind() const;
  size_t getLiteralCount() const;

  SPIRVDecorationGroup *getOwner() const { return Owner; }

  void setOwner(SPIRVDecorationGroup *Owner) { this->Owner = Owner; }

  SPIRVCapVec getRequiredCapability() const override { return getCapability(Dec); }

  SPIRVWord getRequiredSPIRVVersion() const override {
    switch (Dec) {
    case DecorationMaxByteOffset:
      return SPIRV_1_1;

    default:
      return SPIRV_1_0;
    }
  }

protected:
  Decoration Dec;
  std::vector<SPIRVWord> Literals;
  std::vector<SPIRVId> Ids;
  SPIRVDecorationGroup *Owner; // Owning decorate group
};

typedef std::vector<SPIRVDecorateGeneric *> SPIRVDecorateVec;

class SPIRVDecorate : public SPIRVDecorateGeneric {
public:
  static const Op OC = OpDecorate;
  static const SPIRVWord FixedWC = 3;
  // Complete constructor for decorations without literals
  SPIRVDecorate(Decoration TheDec, SPIRVEntry *TheTarget) : SPIRVDecorateGeneric(OC, 3, TheDec, TheTarget) {}
  // Complete constructor for decorations with one word literal
  SPIRVDecorate(Decoration TheDec, SPIRVEntry *TheTarget, SPIRVWord V)
      : SPIRVDecorateGeneric(OC, 4, TheDec, TheTarget, V) {}
  // Incomplete constructor
  SPIRVDecorate() : SPIRVDecorateGeneric(OC) {}

  _SPIRV_DCL_DECODE
  void setWordCount(SPIRVWord) override;
  void validate() const override {
    SPIRVDecorateGeneric::validate();
    assert(WordCount == Literals.size() + FixedWC);
  }
};

class SPIRVDecorateId : public SPIRVDecorate {
public:
  static const Op OC = OpDecorateId;
  static const SPIRVWord FixedWC = 3;

  _SPIRV_DCL_DECODE
  void setWordCount(SPIRVWord) override;
  void validate() const override {
    SPIRVDecorateGeneric::validate();
    assert(WordCount == Ids.size() + FixedWC);
  }
};

class SPIRVDecorateLinkageAttr : public SPIRVDecorate {
public:
  // Complete constructor for LinkageAttributes decorations
  SPIRVDecorateLinkageAttr(SPIRVEntry *TheTarget, const std::string &Name, SPIRVLinkageTypeKind Kind)
      : SPIRVDecorate(DecorationLinkageAttributes, TheTarget) {
    for (auto &I : getVec(Name))
      Literals.push_back(I);
    Literals.push_back(Kind);
    WordCount += Literals.size();
  }
  // Incomplete constructor
  SPIRVDecorateLinkageAttr() : SPIRVDecorate() {}

  std::string getLinkageName() const { return getString(Literals.cbegin(), Literals.cend() - 1); }
  SPIRVLinkageTypeKind getLinkageType() const { return (SPIRVLinkageTypeKind)Literals.back(); }

  static void decodeLiterals(SPIRVDecoder &Decoder, std::vector<SPIRVWord> &Literals) { Decoder >> Literals; }
};

typedef SPIRVDecorate SPIRVDecorateStringGOOGLE;

class SPIRVMemberDecorate : public SPIRVDecorateGeneric {
public:
  static const Op OC = OpMemberDecorate;
  static const SPIRVWord FixedWC = 4;
  // Complete constructor for decorations without literals
  SPIRVMemberDecorate(Decoration TheDec, SPIRVWord Member, SPIRVEntry *TheTarget)
      : SPIRVDecorateGeneric(OC, 4, TheDec, TheTarget), MemberNumber(Member) {}

  // Complete constructor for decorations with one word literal
  SPIRVMemberDecorate(Decoration TheDec, SPIRVWord Member, SPIRVEntry *TheTarget, SPIRVWord V)
      : SPIRVDecorateGeneric(OC, 5, TheDec, TheTarget, V), MemberNumber(Member) {}

  // Incomplete constructor
  SPIRVMemberDecorate() : SPIRVDecorateGeneric(OC), MemberNumber(SPIRVWORD_MAX) {}

  SPIRVWord getMemberNumber() const { return MemberNumber; }
  std::pair<SPIRVWord, Decoration> getPair() const { return std::make_pair(MemberNumber, Dec); }

  _SPIRV_DCL_DECODE
  void setWordCount(SPIRVWord) override;

  void validate() const override {
    SPIRVDecorateGeneric::validate();
    assert(WordCount == Literals.size() + FixedWC);
  }

protected:
  SPIRVWord MemberNumber;
};

typedef SPIRVMemberDecorate SPIRVMemberDecorateStringGOOGLE;

class SPIRVDecorationGroup : public SPIRVEntry {
public:
  static const Op OC = OpDecorationGroup;
  static const SPIRVWord WC = 2;
  // Complete constructor. Does not populate Decorations.
  SPIRVDecorationGroup(SPIRVModule *TheModule, SPIRVId TheId) : SPIRVEntry(TheModule, WC, OC, TheId) { validate(); };
  // Incomplete constructor
  SPIRVDecorationGroup() : SPIRVEntry(OC) {}
  _SPIRV_DCL_DECODE
  // Move the given decorates to the decoration group
  void takeDecorates(SPIRVDecorateVec &Decs) {
    for (SPIRVDecorateVec::const_iterator Dec = Decs.begin(); Dec != Decs.end();) {
      if ((*Dec)->getTargetId() == Id) {
        (*Dec)->setOwner(this);
        Decorations.push_back(*Dec);
        Dec = Decs.erase(Dec); // Remove the decoration from original collection
      } else {
        ++Dec;
      }
    }
  }

  SPIRVDecorateVec &getDecorations() { return Decorations; }

protected:
  SPIRVDecorateVec Decorations;
  void validate() const override {
    assert(OpCode == OC);
    assert(WordCount == WC);
  }
};

class SPIRVGroupDecorateGeneric : public SPIRVEntryNoIdGeneric {
public:
  static const SPIRVWord FixedWC = 2;
  // Complete constructor
  SPIRVGroupDecorateGeneric(Op OC, SPIRVDecorationGroup *TheGroup, const std::vector<SPIRVId> &TheTargets)
      : SPIRVEntryNoIdGeneric(TheGroup->getModule(), FixedWC + TheTargets.size(), OC), DecorationGroup(TheGroup),
        Targets(TheTargets) {}
  // Incomplete constructor
  SPIRVGroupDecorateGeneric(Op OC) : SPIRVEntryNoIdGeneric(OC), DecorationGroup(nullptr) {}

  virtual void decorateTargets() = 0;

protected:
  SPIRVDecorationGroup *DecorationGroup;
  std::vector<SPIRVId> Targets;
};

class SPIRVGroupDecorate : public SPIRVGroupDecorateGeneric {
public:
  static const Op OC = OpGroupDecorate;
  // Complete constructor
  SPIRVGroupDecorate(SPIRVDecorationGroup *TheGroup, const std::vector<SPIRVId> &TheTargets)
      : SPIRVGroupDecorateGeneric(OC, TheGroup, TheTargets) {}
  // Incomplete constructor
  SPIRVGroupDecorate() : SPIRVGroupDecorateGeneric(OC) {}

  void setWordCount(SPIRVWord WC) override {
    SPIRVEntryNoIdGeneric::setWordCount(WC);
    Targets.resize(WC - FixedWC);
  }
  virtual void decorateTargets() override;
  _SPIRV_DCL_DECODE
};

class SPIRVGroupMemberDecorate : public SPIRVGroupDecorateGeneric {
public:
  static const Op OC = OpGroupMemberDecorate;
  // Complete constructor
  SPIRVGroupMemberDecorate(SPIRVDecorationGroup *TheGroup, const std::vector<SPIRVId> &TheTargets)
      : SPIRVGroupDecorateGeneric(OC, TheGroup, TheTargets) {}
  // Incomplete constructor
  SPIRVGroupMemberDecorate() : SPIRVGroupDecorateGeneric(OC) {}

  void setWordCount(SPIRVWord WC) override { SPIRVEntryNoIdGeneric::setWordCount(WC); }
  virtual void decorateTargets() override;
  _SPIRV_DCL_DECODE
protected:
  std::vector<SPIRVWord> MemberNumbers;
};

} // namespace SPIRV

#endif
