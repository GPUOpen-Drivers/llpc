//===- SPIRVEntry.h - Base Class for SPIR-V Entities -------------*- C++ -*-===//
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
/// This file defines the base class for SPIRV entities.
///
//===----------------------------------------------------------------------===//

#ifndef SPIRV_LIBSPIRV_SPIRVENTRY_H
#define SPIRV_LIBSPIRV_SPIRVENTRY_H

#include "SPIRVEnum.h"
#include "SPIRVError.h"
#include "SPIRVIsValidEnum.h"
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace SPIRV {

class SPIRVModule;
class SPIRVEncoder;
class SPIRVDecoder;
class SPIRVType;
class SPIRVValue;
class SPIRVDecorate;
class SPIRVForward;
class SPIRVMemberDecorate;
class SPIRVLine;
class SPIRVString;
class SPIRVExtInst;

// Add declaration of decode functions to a class.
// Used inside class definition.
#define _SPIRV_DCL_DECODE void decode(std::istream &I) override;

#define _REQ_SPIRV_VER(Version)                                                                                        \
  SPIRVWord getRequiredSPIRVVersion() const override { return Version; }

// Add implementation of decode functions to a class.
// Used out side of class definition.
#define _SPIRV_IMP_DECODE0(Ty)                                                                                         \
  void Ty::decode(std::istream &I) {}
#define _SPIRV_IMP_DECODE1(Ty, x)                                                                                      \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x); }
#define _SPIRV_IMP_ENCDEC2(Ty, x, y)                                                                                   \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x) >> (y); }
#define _SPIRV_IMP_DECODE3(Ty, x, y, z)                                                                                \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x) >> (y) >> (z); }
#define _SPIRV_IMP_DECODE4(Ty, x, y, z, u)                                                                             \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x) >> (y) >> (z) >> (u); }
#define _SPIRV_IMP_DECODE5(Ty, x, y, z, u, v)                                                                          \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v); }
#define _SPIRV_IMP_DECODE6(Ty, x, y, z, u, v, w)                                                                       \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v) >> (w); }
#define _SPIRV_IMP_DECODE7(Ty, x, y, z, u, v, w, r)                                                                    \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v) >> (w) >> (r); }
#define _SPIRV_IMP_DECODE8(Ty, x, y, z, u, v, w, r, s)                                                                 \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v) >> (w) >> (r) >> (s); }
#define _SPIRV_IMP_DECODE9(Ty, x, y, z, u, v, w, r, s, t)                                                              \
  void Ty::decode(std::istream &I) { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v) >> (w) >> (r) >> (s) >> (t); }

// Add definition of encode/decode functions to a class.
// Used inside class definition.
#define _SPIRV_DEF_DECODE0                                                                                             \
  void decode(std::istream &I) override {}
#define _SPIRV_DEF_DECODE1(x)                                                                                          \
  void decode(std::istream &I) override { getDecoder(I) >> (x); }
#define _SPIRV_DEF_DECODE2(x, y)                                                                                       \
  void decode(std::istream &I) override { getDecoder(I) >> (x) >> (y); }
#define _SPIRV_DEF_DECODE3(x, y, z)                                                                                    \
  void decode(std::istream &I) override { getDecoder(I) >> (x) >> (y) >> (z); }
#define _SPIRV_DEF_DECODE4(x, y, z, u)                                                                                 \
  void decode(std::istream &I) override { getDecoder(I) >> (x) >> (y) >> (z) >> (u); }
#define _SPIRV_DEF_DECODE5(x, y, z, u, v)                                                                              \
  void decode(std::istream &I) override { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v); }
#define _SPIRV_DEF_DECODE6(x, y, z, u, v, w)                                                                           \
  void decode(std::istream &I) override { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v) >> (w); }
#define _SPIRV_DEF_DECODE7(x, y, z, u, v, w, r)                                                                        \
  void decode(std::istream &I) override { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v) >> (w) >> (r); }
#define _SPIRV_DEF_DECODE8(x, y, z, u, v, w, r, s)                                                                     \
  void decode(std::istream &I) override { getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v) >> (w) >> (r) >> (s); }
#define _SPIRV_DEF_DECODE9(x, y, z, u, v, w, r, s, t)                                                                  \
  void decode(std::istream &I) override {                                                                              \
    getDecoder(I) >> (x) >> (y) >> (z) >> (u) >> (v) >> (w) >> (r) >> (s) >> (t);                                      \
  }

/// All SPIR-V in-memory-representation entities inherits from SPIRVEntry.
/// Usually there are two flavors of constructors of SPIRV objects:
///
/// 1. complete constructor: It requires all the parameters needed to create a
///    SPIRV entity with complete information which can be validated. It is
///    usually used by LLVM/SPIR-V translator to create SPIRV object
///    corresponding to LLVM object. Such constructor calls validate() at
///    the end of the construction.
///
/// 2. incomplete constructor: For leaf classes, it has no parameters.
///    It is usually called by SPIRVEntry::make(opcode) to create an incomplete
///    object which should not be validated. Then setWordCount(count) is
///    called to fix the size of the object if it is variable, and then the
///    information is filled by the virtual function decode(istream).
///    After that the object can be validated.
///
/// To add a new SPIRV class:
///
/// 1. It is recommended to name the class as SPIRVXXX if it has a fixed op code
///    OpXXX. Although it is not mandatory, doing this facilitates adding it to
///    the table of the factory function SPIRVEntry::create().
/// 2. Inherit from proper SPIRV class such as SPIRVType, SPIRVValue,
///    SPIRVInstruction, etc.
/// 3. Implement virtual function decode(), validate().
/// 4. If the object has variable size, implement virtual function
///    setWordCount().
/// 5. If the class has special attributes, e.g. having no id, or having no
///    type as a value, set them in the constructors.
/// 6. If the class may represent SPIRV entity which has been added in version
///    later than 1.0, implement virtual function getRequiredSPIRVVersion().
///    To automatically update module's version you can also call protected
///    function updateModuleVersion() in the constructor.
/// 7. Add the class to the Table of SPIRVEntry::create().
/// 8. Add the class to SPIRVToLLVM.

class SPIRVEntry {
public:
  enum SPIRVEntryAttrib {
    SPIRVEA_DEFAULT = 0,
    SPIRVEA_NOID = 1,   // Entry has no valid id
    SPIRVEA_NOTYPE = 2, // Value has no type
  };

  // Complete constructor for objects with id
  SPIRVEntry(SPIRVModule *M, unsigned TheWordCount, Op TheOpCode, SPIRVId TheId)
      : Module(M), OpCode(TheOpCode), Id(TheId), Attrib(SPIRVEA_DEFAULT), WordCount(TheWordCount), Line(nullptr) {
    validate();
  }

  // Complete constructor for objects without id
  SPIRVEntry(SPIRVModule *M, unsigned TheWordCount, Op TheOpCode)
      : Module(M), OpCode(TheOpCode), Id(SPIRVID_INVALID), Attrib(SPIRVEA_NOID), WordCount(TheWordCount),
        Line(nullptr) {
    validate();
  }

  // Incomplete constructor
  SPIRVEntry(Op TheOpCode)
      : Module(NULL), OpCode(TheOpCode), Id(SPIRVID_INVALID), Attrib(SPIRVEA_DEFAULT), WordCount(0), Line(nullptr) {}

  SPIRVEntry()
      : Module(NULL), OpCode(OpNop), Id(SPIRVID_INVALID), Attrib(SPIRVEA_DEFAULT), WordCount(0), Line(nullptr) {}

  virtual ~SPIRVEntry() {}

  bool exist(SPIRVId) const;
  template <class T> T *get(SPIRVId TheId) const { return reinterpret_cast<T *>(getEntry(TheId)); }
  SPIRVEntry *getEntry(SPIRVId) const;
  SPIRVEntry *getOrCreate(SPIRVId TheId) const;
  SPIRVValue *getValue(SPIRVId TheId) const;
  std::vector<SPIRVValue *> getValues(const std::vector<SPIRVId> &) const;
  std::vector<SPIRVId> getIds(const std::vector<SPIRVValue *> &) const;
  SPIRVType *getValueType(SPIRVId TheId) const;
  std::vector<SPIRVType *> getValueTypes(const std::vector<SPIRVId> &) const;

  virtual SPIRVDecoder getDecoder(std::istream &);
  SPIRVErrorLog &getErrorLog() const;
  SPIRVId getId() const {
    assert(hasId());
    return Id;
  }
  const SPIRVLine *getLine() const { return Line; }
  SPIRVLinkageTypeKind getLinkageType() const;
  Op getOpCode() const { return OpCode; }
  SPIRVModule *getModule() const { return Module; }
  virtual SPIRVCapVec getRequiredCapability() const { return SPIRVCapVec(); }
  const std::string &getName() const { return Name; }
  bool hasDecorate(Decoration Kind, size_t Index = 0, SPIRVWord *Result = 0) const;
  SPIRVEntry *getDecorateId(Decoration Kind, size_t Index) const;
  const char *getDecorateString(Decoration kind) const;
  bool hasMemberDecorate(SPIRVWord MemberIndex, Decoration Kind, size_t Index = 0, SPIRVWord *Result = 0) const;
  std::set<SPIRVWord> getDecorate(Decoration Kind, size_t Index = 0) const;
  bool hasId() const { return !(Attrib & SPIRVEA_NOID); }
  bool hasLine() const { return Line != nullptr; }
  bool hasLinkageType() const;
  bool isAtomic() const { return isAtomicOpCode(OpCode); }
  bool isBasicBlock() const { return isLabel(); }
  bool isExtInst() const { return OpCode == OpExtInst; }
  bool isExtInst(const SPIRVExtInstSetKind InstSet, const SPIRVWord ExtOp) const;
  bool isBuiltinCall() const { return OpCode == OpExtInst; }
  bool isDecorate() const { return OpCode == OpDecorate; }
  bool isMemberDecorate() const { return OpCode == OpMemberDecorate; }
  bool isForward() const { return OpCode == OpForward; }
  bool isLabel() const { return OpCode == OpLabel; }
  bool isUndef() const { return OpCode == OpUndef; }
  bool isControlBarrier() const { return OpCode == OpControlBarrier; }
  bool isMemoryBarrier() const { return OpCode == OpMemoryBarrier; }
  bool isVariable() const { return OpCode == OpVariable; }
  bool isEndOfBlock() const;
  virtual bool isInst() const { return false; }
  virtual bool isOperandLiteral(unsigned Index) const {
    assert(0 && "not implemented");
    return false;
  }

  void addDecorate(const SPIRVDecorate *);
  void addDecorate(Decoration Kind);
  void addDecorate(Decoration Kind, SPIRVWord Literal);
  void eraseDecorate(Decoration);
  void addMemberDecorate(const SPIRVMemberDecorate *);
  void addMemberDecorate(SPIRVWord MemberNumber, Decoration Kind);
  void addMemberDecorate(SPIRVWord MemberNumber, Decoration Kind, SPIRVWord Literal);
  void eraseMemberDecorate(SPIRVWord MemberNumber, Decoration Kind);
  void setHasNoId() { Attrib |= SPIRVEA_NOID; }
  void setId(SPIRVId TheId) { Id = TheId; }
  void setLine(const SPIRVLine *L);
  void setLinkageType(SPIRVLinkageTypeKind);
  void setModule(SPIRVModule *TheModule);
  void setName(const std::string &TheName);
  virtual void setScope(SPIRVEntry *Scope){};
  void takeAnnotations(SPIRVForward *);
  void takeDecorates(SPIRVEntry *);
  void takeMemberDecorates(SPIRVEntry *);

  /// After a SPIRV entry is created during reading SPIRV binary by default
  /// constructor, this function is called to allow the SPIRV entry to resize
  /// its variable sized member before decoding the remaining words.
  virtual void setWordCount(SPIRVWord TheWordCount);

  /// Create an empty SPIRV object by op code, e.g. OpTypeInt creates
  /// SPIRVTypeInt.
  static SPIRVEntry *create(Op);
  static std::unique_ptr<SPIRVEntry> createUnique(Op);

  /// Create an empty extended instruction.
  static std::unique_ptr<SPIRVExtInst> createUnique(SPIRVExtInstSetKind Set, unsigned ExtOp);

  friend std::istream &operator>>(std::istream &I, SPIRVEntry &E);
  virtual void decode(std::istream &I);

  friend class SPIRVDecoder;

  /// Checks the integrity of the object.
  virtual void validate() const {
    assert(Module && "Invalid module");
    assert(OpCode != OpNop && "Invalid op code");
    assert((!hasId() || isValidId(Id)) && "Invalid Id");
  }
  void validateFunctionControlMask(SPIRVWord FCtlMask) const;
  void validateValues(const std::vector<SPIRVId> &) const;
  void validateBuiltin(SPIRVWord, SPIRVWord) const;

  // By default assume SPIRV 1.0 as required version
  virtual SPIRVWord getRequiredSPIRVVersion() const { return SPIRV_1_0; }

  virtual std::vector<SPIRVEntry *> getNonLiteralOperands() const { return std::vector<SPIRVEntry *>(); }

protected:
  /// An entry may have multiple FuncParamAttr decorations.
  typedef std::multimap<Decoration, const SPIRVDecorate *> DecorateMapType;

  struct DectorateKeyHash {
    using DecorationInt = std::underlying_type<Decoration>::type;
    std::hash<SPIRVWord> wordHash;
    std::hash<DecorationInt> decorationHash;
    size_t operator()(const std::pair<SPIRVWord, Decoration> &key) const {
      return wordHash(key.first) ^ decorationHash(static_cast<DecorationInt>(key.second));
    }
  };
  typedef std::unordered_map<std::pair<SPIRVWord, Decoration>, const SPIRVMemberDecorate *, DectorateKeyHash>
      MemberDecorateMapType;

  bool canHaveMemberDecorates() const { return OpCode == OpTypeStruct || OpCode == OpForward; }
  MemberDecorateMapType &getMemberDecorates() {
    assert(canHaveMemberDecorates());
    return MemberDecorates;
  }

  void updateModuleVersion() const;

  SPIRVModule *Module;
  Op OpCode;
  SPIRVId Id;
  std::string Name;
  unsigned Attrib;
  SPIRVWord WordCount;

  DecorateMapType Decorates;
  MemberDecorateMapType MemberDecorates;
  const SPIRVLine *Line;
};

class SPIRVEntryNoIdGeneric : public SPIRVEntry {
public:
  SPIRVEntryNoIdGeneric(SPIRVModule *M, unsigned TheWordCount, Op OC) : SPIRVEntry(M, TheWordCount, OC) { setAttr(); }
  SPIRVEntryNoIdGeneric(Op OC) : SPIRVEntry(OC) { setAttr(); }

protected:
  void setAttr() { setHasNoId(); }
};

template <Op OC> class SPIRVEntryNoId : public SPIRVEntryNoIdGeneric {
public:
  SPIRVEntryNoId(SPIRVModule *M, unsigned TheWordCount) : SPIRVEntryNoIdGeneric(M, TheWordCount, OC) {}
  SPIRVEntryNoId() : SPIRVEntryNoIdGeneric(OC) {}
};

template <Op TheOpCode> class SPIRVEntryOpCodeOnly : public SPIRVEntryNoId<TheOpCode> {
public:
  SPIRVEntryOpCodeOnly() {
    SPIRVEntry::WordCount = 1;
    validate();
  }

protected:
  _SPIRV_DEF_DECODE0
  void validate() const override { assert(isValidId(SPIRVEntry::OpCode)); }
};

class SPIRVAnnotationGeneric : public SPIRVEntryNoIdGeneric {
public:
  // Complete constructor
  SPIRVAnnotationGeneric(SPIRVModule *TheModule, unsigned TheWordCount, Op OC, SPIRVId TheTarget = SPIRVID_INVALID)
      : SPIRVEntryNoIdGeneric(TheModule, TheWordCount, OC), Target(TheTarget) {}
  // Incomplete constructor
  SPIRVAnnotationGeneric(Op OC) : SPIRVEntryNoIdGeneric(OC), Target(SPIRVID_INVALID) {}

  SPIRVId getTargetId() const { return Target; }
  SPIRVForward *getOrCreateTarget() const;
  void setTargetId(SPIRVId T) { Target = T; }

protected:
  SPIRVId Target;
};

template <Op OC> class SPIRVAnnotation : public SPIRVAnnotationGeneric {
public:
  // Complete constructor
  SPIRVAnnotation(const SPIRVEntry *TheTarget, unsigned TheWordCount)
      : SPIRVAnnotationGeneric(TheTarget->getModule(), TheWordCount, OC, TheTarget->getId()) {}
  // Incomplete constructor
  SPIRVAnnotation() : SPIRVAnnotationGeneric(OC) {}
};

class SPIRVEntryPoint : public SPIRVAnnotation<OpEntryPoint> {
public:
  SPIRVEntryPoint(SPIRVModule *TheModule, SPIRVExecutionModelKind, SPIRVId TheId, const std::string &TheName);
  SPIRVEntryPoint() : ExecModel(ExecutionModelVertex) {}
  SPIRVExecutionModelKind getExecModel() const { return ExecModel; }
  std::string getName() const { return Name; }
  std::pair<const SPIRVWord *, size_t> getInOuts() const { return {InOuts.data(), InOuts.size()}; }
  _SPIRV_DCL_DECODE
protected:
  SPIRVExecutionModelKind ExecModel;
  std::string Name;
  std::vector<SPIRVWord> InOuts;
};

class SPIRVName : public SPIRVAnnotation<OpName> {
public:
  // Complete constructor
  SPIRVName(const SPIRVEntry *TheTarget, const std::string &TheStr);
  // Incomplete constructor
  SPIRVName() {}

protected:
  _SPIRV_DCL_DECODE
  void validate() const override;

  std::string Str;
};

class SPIRVMemberName : public SPIRVAnnotation<OpName> {
public:
  static const SPIRVWord FixedWC = 3;
  // Complete constructor
  SPIRVMemberName(const SPIRVEntry *TheTarget, SPIRVWord TheMemberNumber, const std::string &TheStr)
      : SPIRVAnnotation(TheTarget, FixedWC + getSizeInWords(TheStr)), MemberNumber(TheMemberNumber), Str(TheStr) {
    validate();
  }
  // Incomplete constructor
  SPIRVMemberName() : MemberNumber(SPIRVWORD_MAX) {}

protected:
  _SPIRV_DCL_DECODE
  void validate() const override;
  SPIRVWord MemberNumber;
  std::string Str;
};

class SPIRVString : public SPIRVEntry {
  static const Op OC = OpString;
  static const SPIRVWord FixedWC = 2;

public:
  SPIRVString(SPIRVModule *M, SPIRVId TheId, const std::string &TheStr)
      : SPIRVEntry(M, FixedWC + getSizeInWords(TheStr), OC, TheId), Str(TheStr) {}
  SPIRVString() : SPIRVEntry(OC) {}
  _SPIRV_DCL_DECODE
  const std::string &getStr() const { return Str; }

protected:
  std::string Str;
};

class SPIRVLine : public SPIRVEntryNoId<OpLine> {
public:
  // Complete constructor
  SPIRVLine(SPIRVModule *M, SPIRVId TheFileName, SPIRVWord TheLine, SPIRVWord TheColumn)
      : SPIRVEntryNoId(M, 4), FileName(TheFileName), Line(TheLine), Column(TheColumn) {
    Attrib = SPIRVEA_NOID | SPIRVEA_NOTYPE;
    validate();
  }
  // Incomplete constructor
  SPIRVLine() : SPIRVEntryNoId(), FileName(SPIRVID_INVALID), Line(SPIRVWORD_MAX), Column(SPIRVWORD_MAX) {
    Attrib = SPIRVEA_NOID | SPIRVEA_NOTYPE;
  }

  SPIRVWord getColumn() const { return Column; }

  void setColumn(const SPIRVWord Column) { this->Column = Column; }

  SPIRVId getFileName() const { return FileName; }

  const std::string &getFileNameStr() const { return get<SPIRVString>(FileName)->getStr(); }

  void setFileName(const SPIRVId FileName) { this->FileName = FileName; }

  SPIRVWord getLine() const { return Line; }

  void setLine(const SPIRVWord Line) { this->Line = Line; }

  bool operator!=(const SPIRVLine &O) const { return !equals(O.FileName, O.Line, O.Column); }

  bool equals(const SPIRVId TheFileName, const SPIRVWord TheLine, const SPIRVWord TheColumn) const {
    return FileName == TheFileName && Line == TheLine && Column == TheColumn;
  }

protected:
  _SPIRV_DCL_DECODE
  void validate() const override;
  SPIRVId FileName;
  SPIRVWord Line;
  SPIRVWord Column;
};

typedef SPIRVEntryOpCodeOnly<OpNoLine> SPIRVNoLine;

class SPIRVExecutionMode : public SPIRVAnnotation<OpExecutionMode> {
public:
  // Complete constructor for LocalSize
  SPIRVExecutionMode(SPIRVEntry *TheTarget, SPIRVExecutionModeKind TheExecMode, SPIRVWord Word0, SPIRVWord Word1,
                     SPIRVWord Word2)
      : SPIRVAnnotation(TheTarget, 6), ExecMode(TheExecMode) {
    WordLiterals.push_back(Word0);
    WordLiterals.push_back(Word1);
    WordLiterals.push_back(Word2);
    updateModuleVersion();
  }
  // Complete constructor for SubgroupSize, SubgroupsPerWorkgroup
  SPIRVExecutionMode(SPIRVEntry *TheTarget, SPIRVExecutionModeKind TheExecMode, SPIRVWord Code)
      : SPIRVAnnotation(TheTarget, 4), ExecMode(TheExecMode) {
    WordLiterals.push_back(Code);
    updateModuleVersion();
  }

  // Incomplete constructor
  SPIRVExecutionMode() : ExecMode(ExecutionModeInvocations) {}
  SPIRVExecutionModeKind getExecutionMode() const { return ExecMode; }
  const std::vector<SPIRVWord> &getLiterals() const { return WordLiterals; }
  void updateLiteral(unsigned index, SPIRVWord Literal) {
    assert(index < WordLiterals.size());
    WordLiterals[index] = Literal;
  }
  SPIRVCapVec getRequiredCapability() const override { return getCapability(ExecMode); }

  SPIRVWord getRequiredSPIRVVersion() const override {
    switch (ExecMode) {
    case ExecutionModeSubgroupSize:
    case ExecutionModeSubgroupsPerWorkgroup:
      return SPIRV_1_1;

    default:
      return SPIRV_1_0;
    }
  }

  void mergeExecutionMode(SPIRVExecutionMode *EM) {
    assert(ExecMode == ExecutionModeDenormPreserve || ExecMode == ExecutionModeDenormFlushToZero ||
           ExecMode == ExecutionModeSignedZeroInfNanPreserve || ExecMode == ExecutionModeRoundingModeRTE ||
           ExecMode == ExecutionModeRoundingModeRTZ);
    assert(WordLiterals.size() == 1);
    assert(ExecMode == EM->ExecMode);
    assert(EM->WordLiterals[0] == 16 || EM->WordLiterals[0] == 32 || EM->WordLiterals[0] == 64);
    WordLiterals[0] |= EM->WordLiterals[0];
  }

protected:
  _SPIRV_DCL_DECODE
  SPIRVExecutionModeKind ExecMode;
  std::vector<SPIRVWord> WordLiterals;
};

class SPIRVExecutionModeId : public SPIRVAnnotation<OpExecutionModeId> {
public:
  // Incomplete constructor
  SPIRVExecutionModeId() {}
  _SPIRV_DCL_DECODE
  const std::vector<SPIRVId> &getOperands() const { return Operands; }
  SPIRVExecutionModeKind getExecutionMode() const { return ExecMode; }

protected:
  SPIRVExecutionModeKind ExecMode;
  std::vector<SPIRVId> Operands;
};

class SPIRVComponentExecutionModes {
  typedef std::map<SPIRVExecutionModeKind, SPIRVExecutionMode *> SPIRVExecutionModeMap;

public:
  void addExecutionMode(SPIRVExecutionMode *ExecMode) { ExecModes[ExecMode->getExecutionMode()] = ExecMode; }

  void mergeExecutionMode(SPIRVExecutionMode *ExecMode) {
    auto EMKind = ExecMode->getExecutionMode();
    auto OrigExecMode = getExecutionMode(EMKind);
    if (OrigExecMode != nullptr)
      OrigExecMode->mergeExecutionMode(ExecMode);
    else
      ExecModes[EMKind] = ExecMode;
  }

  SPIRVExecutionMode *getExecutionMode(SPIRVExecutionModeKind EMK) const {
    auto Loc = ExecModes.find(EMK);
    if (Loc == ExecModes.end())
      return nullptr;
    return Loc->second;
  }

protected:
  SPIRVExecutionModeMap ExecModes;
};

class SPIRVExtInstImport : public SPIRVEntry {
public:
  const static Op OC = OpExtInstImport;
  // Complete constructor
  SPIRVExtInstImport(SPIRVModule *TheModule, SPIRVId TheId, const std::string &TheStr);
  // Incomplete constructor
  SPIRVExtInstImport() : SPIRVEntry(OC) {}

protected:
  _SPIRV_DCL_DECODE
  void validate() const override;

  std::string Str;
};

class SPIRVMemoryModel : public SPIRVEntryNoId<OpMemoryModel> {
public:
  SPIRVMemoryModel(SPIRVModule *M) : SPIRVEntryNoId(M, 3) {}
  SPIRVMemoryModel() {}
  _SPIRV_DCL_DECODE
  void validate() const override;
};

class SPIRVSource : public SPIRVEntryNoId<OpSource> {
public:
  SPIRVSource(SPIRVModule *M) : SPIRVEntryNoId(M, 3) {}
  SPIRVSource() {}
  _SPIRV_DCL_DECODE
private:
  SPIRVId File;
  std::string Source;
};

class SPIRVSourceContinued : public SPIRVEntryNoId<OpSourceContinued> {
public:
  SPIRVSourceContinued(SPIRVModule *M, const std::string &SS);
  SPIRVSourceContinued() {}
  _SPIRV_DCL_DECODE
private:
  std::string Str;
};

class SPIRVSourceExtension : public SPIRVEntryNoId<OpSourceExtension> {
public:
  SPIRVSourceExtension(SPIRVModule *M, const std::string &SS);
  SPIRVSourceExtension() {}
  _SPIRV_DCL_DECODE
private:
  std::string S;
};

class SPIRVExtension : public SPIRVEntryNoId<OpExtension> {
public:
  SPIRVExtension(SPIRVModule *M, const std::string &SS);
  SPIRVExtension() {}
  _SPIRV_DCL_DECODE
private:
  std::string S;
};

class SPIRVCapability : public SPIRVEntryNoId<OpCapability> {
public:
  SPIRVCapability(SPIRVModule *M, SPIRVCapabilityKind K);
  SPIRVCapability() : Kind(CapabilityMatrix) {}
  _SPIRV_DCL_DECODE

  SPIRVWord getRequiredSPIRVVersion() const override {
    switch (Kind) {
    case CapabilityNamedBarrier:
    case CapabilitySubgroupDispatch:
      return SPIRV_1_1;

    default:
      return SPIRV_1_0;
    }
  }

private:
  SPIRVCapabilityKind Kind;
};

class SPIRVModuleProcessed : public SPIRVEntryNoId<OpModuleProcessed> {
public:
  SPIRVModuleProcessed(SPIRVModule *M, const std::string &SS);
  SPIRVModuleProcessed() {}
  _SPIRV_DCL_DECODE
private:
  std::string Str;
};

typedef SPIRVEntryOpCodeOnly<OpNop> SPIRVNop; // Do nothing

template <class T> T *bcast(SPIRVEntry *E) {
  return static_cast<T *>(E);
}

template <spv::Op OC> bool isa(SPIRVEntry *E) {
  return E ? E->getOpCode() == OC : false;
}
// ToDo: The following typedef's are place holders for SPIRV entity classes
// to be implemented.
// Each time a new class is implemented, remove the corresponding typedef.
// This is also an indication of how much work is left.
#define _SPIRV_OP(x, ...) typedef SPIRVEntryOpCodeOnly<Op##x> SPIRV##x;
_SPIRV_OP(SizeOf)
// NOTE: These 4 OpCodes are reserved by SPIR-V spec, they are invalid unless
// some extensions expose them.
_SPIRV_OP(ImageSparseSampleProjImplicitLod)
_SPIRV_OP(ImageSparseSampleProjExplicitLod)
_SPIRV_OP(ImageSparseSampleProjDrefImplicitLod)
_SPIRV_OP(ImageSparseSampleProjDrefExplicitLod)
#undef _SPIRV_OP

} // namespace SPIRV
#endif /* SPIRVENTRY_HPP_ */
