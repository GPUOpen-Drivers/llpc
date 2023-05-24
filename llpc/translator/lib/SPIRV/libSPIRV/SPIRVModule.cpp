//===- SPIRVModule.cpp - Class to represent SPIR-V module --------*- C++ -*-===//
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
/// This file implements Module class for SPIR-V.
///
//===----------------------------------------------------------------------===//

#include "SPIRVModule.h"
#include "SPIRVDebug.h"
#include "SPIRVEntry.h"
#include "SPIRVExtInst.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "SPIRVStream.h"
#include "SPIRVType.h"
#include "SPIRVValue.h"
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace SPIRV {

SPIRVModule::SPIRVModule() : AutoAddCapability(true), ValidateCapability(false) {
}

SPIRVModule::~SPIRVModule() {
}

class SPIRVModuleImpl : public SPIRVModule {
public:
  SPIRVModuleImpl()
      : SPIRVModule(), NextId(1), SPIRVVersion(SPIRV_1_0), GeneratorVer(0), InstSchema(SPIRVISCH_Default),
        SrcLang(SourceLanguageGLSL), SrcLangVer(102000), CurrentLine(nullptr) {
    AddrModel = sizeof(size_t) == 32 ? AddressingModelPhysical32 : AddressingModelPhysical64;
    setMemoryModel(MemoryModelGLSL450);
  }
  ~SPIRVModuleImpl() override;

  // Object query functions
  bool exist(SPIRVId) const override;
  bool exist(SPIRVId, SPIRVEntry **) const override;
  SPIRVId getId(SPIRVId Id = SPIRVID_INVALID, unsigned Increment = 1);
  SPIRVEntry *getEntry(SPIRVId Id) const override;
  // If there's at least one OpLine in the module the CurrentLine is non-empty.
  bool hasDebugInfo() const override { return CurrentLine != nullptr || !StringVec.empty() || !DebugInstVec.empty(); }

  // Error handling functions
  SPIRVErrorLog &getErrorLog() override { return ErrLog; }
  SPIRVErrorCode getError(std::string &ErrMsg) override { return ErrLog.getError(ErrMsg); }

  // Module query functions
  SPIRVAddressingModelKind getAddressingModel() override { return AddrModel; }
  SPIRVExtInstSetKind getBuiltinSet(SPIRVId SetId) const override;
  const SPIRVCapMap &getCapability() const override { return CapMap; }
  bool hasCapability(SPIRVCapabilityKind Cap) const override { return CapMap.find(Cap) != CapMap.end(); }
  std::set<std::string> &getExtension() override { return SPIRVExt; }
  SPIRVFunction *getFunction(unsigned I) const override { return FuncVec[I]; }
  SPIRVVariable *getVariable(unsigned I) const override { return VariableVec[I]; }
  const std::vector<SPIRVString *> &getStringVec() const override { return StringVec; }
  SPIRVValue *getConstant(unsigned I) const override { return ConstVec[I]; }
  SPIRVValue *getValue(SPIRVId TheId) const override;
  std::vector<SPIRVValue *> getValues(const std::vector<SPIRVId> &) const override;
  std::vector<SPIRVId> getIds(const std::vector<SPIRVEntry *> &) const override;
  std::vector<SPIRVId> getIds(const std::vector<SPIRVValue *> &) const override;
  SPIRVType *getValueType(SPIRVId TheId) const override;
  std::vector<SPIRVType *> getValueTypes(const std::vector<SPIRVId> &) const override;
  SPIRVMemoryModelKind getMemoryModel() const override { return MemoryModel; }
  SPIRVConstant *getLiteralAsConstant(unsigned Literal) override;
  unsigned getNumEntryPoints(SPIRVExecutionModelKind EM) const override {
    unsigned NumEntryPoint = 0;
    for (auto EntryPoint : EntryPointVec) {
      if (EntryPoint->getExecModel() == EM)
        ++NumEntryPoint;
    }
    return NumEntryPoint;
  }
  SPIRVFunction *getEntryPoint(SPIRVExecutionModelKind EM, unsigned I) const override {
    unsigned EntryPointIndex = 0;
    for (auto EntryPoint : EntryPointVec) {
      if (EntryPoint->getExecModel() == EM) {
        if (EntryPointIndex == I)
          return get<SPIRVFunction>(EntryPoint->getTargetId());
        ++EntryPointIndex;
      }
    }
    return nullptr;
  }
  SPIRVExecutionModelKind getExecutionModel() const override {
    return EntryPointVec.size() > 0 ? EntryPointVec[0]->getExecModel() : ExecutionModelMax;
  }
  unsigned getNumFunctions() const override { return FuncVec.size(); }
  unsigned getNumVariables() const override { return VariableVec.size(); }
  unsigned getNumConstants() const override { return ConstVec.size(); }
  SourceLanguage getSourceLanguage(SPIRVWord *Ver = nullptr) const override {
    if (Ver)
      *Ver = SrcLangVer;
    return SrcLang;
  }

  SPIRVString *getSourceFile(uint32_t FileId) const override {
    return SrcFiles.size() > FileId ? get<SPIRVString>(SrcFiles[FileId]) : nullptr;
  }

  std::set<std::string> &getSourceExtension() override { return SrcExtension; }
  virtual SPIRVEntryPoint *getEntryPoint(SPIRVId) const override;
  virtual SPIRVEntryPoint *getEntryPoint(SPIRVExecutionModelKind, const char *) const override;
  virtual bool isEntryPoint(SPIRVExecutionModelKind ExecModel, SPIRVId EP) const override;
  unsigned short getGeneratorId() const override { return GeneratorId; }
  unsigned short getGeneratorVer() const override { return GeneratorVer; }
  SPIRVWord getSPIRVVersion() const override { return SPIRVVersion; }
  const std::vector<SPIRVExtInst *> &getDebugInstVec() const override { return DebugInstVec; }
  bool isNonSemanticInfoInstSet(llvm::StringRef setName) const;

  // Module changing functions
  bool importBuiltinSet(const std::string &, SPIRVId *) override;
  bool importBuiltinSetWithId(const std::string &, SPIRVId) override;
  void optimizeDecorates() override;
  void setAddressingModel(SPIRVAddressingModelKind AM) override { AddrModel = AM; }
  void postProcessExecutionModeId();
  void setMemoryModel(SPIRVMemoryModelKind MM) override { MemoryModel = MM; }
  void setName(SPIRVEntry *E, const std::string &Name) override;
  void setSourceLanguage(SourceLanguage Lang, SPIRVWord Ver) override {
    SrcLang = Lang;
    SrcLangVer = Ver;
  }
  void setSourceFile(SPIRVId File) override { SrcFiles.push_back(File); }
  void setGeneratorId(unsigned short Id) override { GeneratorId = Id; }
  void setGeneratorVer(unsigned short Ver) override { GeneratorVer = Ver; }
  void resolveUnknownStructFields() override;

  void setSPIRVVersion(SPIRVWord Ver) override { SPIRVVersion = Ver; }

  // Object creation functions
  template <class T> void addTo(std::vector<T *> &V, SPIRVEntry *E);
  SPIRVEntry *addEntry(SPIRVEntry *E) override;
  SPIRVBasicBlock *addBasicBlock(SPIRVFunction *, SPIRVId) override;
  SPIRVString *getString(const std::string &Str) override;
  SPIRVMemberName *addMemberName(SPIRVTypeStruct *ST, SPIRVWord MemberNumber, const std::string &Name) override;
  void addUnknownStructField(SPIRVTypeStruct *Struct, unsigned I, SPIRVId ID) override;
  const SPIRVLine *getCurrentLine() const override;
  void setCurrentLine(const SPIRVLine *Line) override;
  void addCapability(SPIRVCapabilityKind) override;
  void addCapabilityInternal(SPIRVCapabilityKind) override;
  const SPIRVDecorateGeneric *addDecorate(const SPIRVDecorateGeneric *) override;
  SPIRVDecorationGroup *addDecorationGroup() override;
  SPIRVDecorationGroup *addDecorationGroup(SPIRVDecorationGroup *Group) override;
  SPIRVGroupDecorate *addGroupDecorate(SPIRVDecorationGroup *Group, const std::vector<SPIRVEntry *> &Targets) override;
  SPIRVGroupDecorateGeneric *addGroupDecorateGeneric(SPIRVGroupDecorateGeneric *GDec) override;
  SPIRVGroupMemberDecorate *addGroupMemberDecorate(SPIRVDecorationGroup *Group,
                                                   const std::vector<SPIRVEntry *> &Targets) override;
  void addEntryPoint(SPIRVEntryPoint *EntryPoint) override;
  SPIRVForward *addForward(SPIRVType *Ty) override;
  SPIRVForward *addForward(SPIRVId, SPIRVType *Ty) override;
  SPIRVFunction *addFunction(SPIRVFunction *) override;
  SPIRVFunction *addFunction(SPIRVTypeFunction *, SPIRVId) override;
  SPIRVEntry *replaceForward(SPIRVForward *, SPIRVEntry *) override;
  void eraseInstruction(SPIRVInstruction *, SPIRVBasicBlock *) override;

  SPIRVEntry *replaceForwardPointer(SPIRVTypeForwardPointer *, SPIRVTypePointer *);

  // Type creation functions
  template <class T> T *addType(T *Ty);
  SPIRVTypeArray *addArrayType(SPIRVType *, SPIRVConstant *) override;
  SPIRVTypeRuntimeArray *addRuntimeArray(SPIRVType *) override;
  SPIRVTypeStruct *addStructType(const std::vector<SPIRVType *> &vecTypes) override;
  SPIRVTypeBool *addBoolType() override;
  SPIRVTypeFloat *addFloatType(unsigned BitWidth) override;
  SPIRVTypeFunction *addFunctionType(SPIRVType *, const std::vector<SPIRVType *> &) override;
  SPIRVTypeInt *addIntegerType(unsigned BitWidth) override;
  SPIRVTypePointer *addPointerType(SPIRVStorageClassKind, SPIRVType *) override;
  SPIRVTypeImage *addImageType(SPIRVType *, const SPIRVTypeImageDescriptor &) override;
  SPIRVTypeSampler *addSamplerType() override;
  SPIRVTypeSampledImage *addSampledImageType(SPIRVTypeImage *T) override;
  SPIRVTypeVector *addVectorType(SPIRVType *, SPIRVWord) override;
  SPIRVTypeVoid *addVoidType() override;
  void createForwardPointers() override;

  // Constant creation functions
  SPIRVInstruction *addBranchInst(SPIRVLabel *, SPIRVBasicBlock *) override;
  SPIRVInstruction *addBranchConditionalInst(SPIRVValue *, SPIRVLabel *, SPIRVLabel *, SPIRVBasicBlock *) override;
  SPIRVValue *addCompositeConstant(SPIRVType *, const std::vector<SPIRVValue *> &) override;
  SPIRVValue *addConstant(SPIRVValue *) override;
  SPIRVValue *addConstant(SPIRVType *, uint64_t) override;
  SPIRVValue *addDoubleConstant(SPIRVTypeFloat *, double) override;
  SPIRVValue *addFloatConstant(SPIRVTypeFloat *, float) override;
  SPIRVValue *addIntegerConstant(SPIRVTypeInt *, uint64_t) override;
  SPIRVValue *addNullConstant(SPIRVType *) override;
  SPIRVValue *addUndef(SPIRVType *TheType) override;

  // Instruction creation functions
  SPIRVInstruction *addPtrAccessChainInst(SPIRVType *, SPIRVValue *, std::vector<SPIRVValue *>, SPIRVBasicBlock *,
                                          bool) override;
  SPIRVInstruction *addExtInst(SPIRVType *, SPIRVWord, SPIRVWord, const std::vector<SPIRVWord> &,
                               SPIRVBasicBlock *) override;
  SPIRVInstruction *addExtInst(SPIRVType *, SPIRVWord, SPIRVWord, const std::vector<SPIRVValue *> &,
                               SPIRVBasicBlock *) override;
  SPIRVInstruction *addBinaryInst(Op, SPIRVType *, SPIRVValue *, SPIRVValue *, SPIRVBasicBlock *) override;
  SPIRVInstruction *addCallInst(SPIRVFunction *, const std::vector<SPIRVWord> &, SPIRVBasicBlock *) override;
  SPIRVInstruction *addCmpInst(Op, SPIRVType *, SPIRVValue *, SPIRVValue *, SPIRVBasicBlock *) override;
  SPIRVInstruction *addLoadInst(SPIRVValue *, const std::vector<SPIRVWord> &, SPIRVBasicBlock *) override;
  SPIRVInstruction *addPhiInst(SPIRVType *, std::vector<SPIRVValue *>, SPIRVBasicBlock *) override;
  SPIRVInstruction *addCompositeConstructInst(SPIRVType *, const std::vector<SPIRVId> &, SPIRVBasicBlock *) override;
  SPIRVInstruction *addCompositeExtractInst(SPIRVType *, SPIRVValue *, const std::vector<SPIRVWord> &,
                                            SPIRVBasicBlock *) override;
  SPIRVInstruction *addCompositeInsertInst(SPIRVValue *Object, SPIRVValue *Composite,
                                           const std::vector<SPIRVWord> &Indices, SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addCopyObjectInst(SPIRVType *TheType, SPIRVValue *Operand, SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addCopyMemoryInst(SPIRVValue *, SPIRVValue *, const std::vector<SPIRVWord> &,
                                      SPIRVBasicBlock *) override;
  SPIRVInstruction *addCopyMemorySizedInst(SPIRVValue *, SPIRVValue *, SPIRVValue *, const std::vector<SPIRVWord> &,
                                           SPIRVBasicBlock *) override;
  SPIRVInstruction *addControlBarrierInst(SPIRVValue *ExecKind, SPIRVValue *MemKind, SPIRVValue *MemSema,
                                          SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addGroupInst(Op OpCode, SPIRVType *Type, Scope Scope, const std::vector<SPIRVValue *> &Ops,
                                 SPIRVBasicBlock *BB) override;
  virtual SPIRVInstruction *addInstruction(SPIRVInstruction *Inst, SPIRVBasicBlock *BB);
  SPIRVInstTemplateBase *addInstTemplate(Op OC, SPIRVBasicBlock *BB, SPIRVType *Ty) override;
  SPIRVInstTemplateBase *addInstTemplate(Op OC, const std::vector<SPIRVWord> &Ops, SPIRVBasicBlock *BB,
                                         SPIRVType *Ty) override;
  SPIRVInstruction *addMemoryBarrierInst(Scope ScopeKind, SPIRVWord MemFlag, SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addUnreachableInst(SPIRVBasicBlock *) override;
  SPIRVInstruction *addReturnInst(SPIRVBasicBlock *) override;
  SPIRVInstruction *addReturnValueInst(SPIRVValue *, SPIRVBasicBlock *) override;
  SPIRVInstruction *addSelectInst(SPIRVValue *, SPIRVValue *, SPIRVValue *, SPIRVBasicBlock *) override;
  SPIRVInstruction *addLoopMergeInst(SPIRVId MergeBlock, SPIRVId ContinueTarget, SPIRVWord LoopControl,
                                     std::vector<SPIRVWord> LoopControlParameters, SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addSelectionMergeInst(SPIRVId MergeBlock, SPIRVWord SelectionControl, SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addStoreInst(SPIRVValue *, SPIRVValue *, const std::vector<SPIRVWord> &,
                                 SPIRVBasicBlock *) override;
  SPIRVInstruction *addSwitchInst(SPIRVValue *, SPIRVBasicBlock *,
                                  const std::vector<std::pair<std::vector<SPIRVWord>, SPIRVBasicBlock *>> &,
                                  SPIRVBasicBlock *) override;
  SPIRVInstruction *addFModInst(SPIRVType *TheType, SPIRVId TheDividend, SPIRVId TheDivisor,
                                SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addVectorTimesScalarInst(SPIRVType *TheType, SPIRVId TheVector, SPIRVId TheScalar,
                                             SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addUnaryInst(Op, SPIRVType *, SPIRVValue *, SPIRVBasicBlock *) override;
  SPIRVInstruction *addVariable(SPIRVType *, bool, SPIRVLinkageTypeKind, SPIRVValue *, const std::string &,
                                SPIRVStorageClassKind, SPIRVBasicBlock *) override;
  SPIRVValue *addVectorShuffleInst(SPIRVType *Type, SPIRVValue *Vec1, SPIRVValue *Vec2,
                                   const std::vector<SPIRVWord> &Components, SPIRVBasicBlock *BB) override;
  SPIRVInstruction *addVectorExtractDynamicInst(SPIRVValue *, SPIRVValue *, SPIRVBasicBlock *) override;
  SPIRVInstruction *addVectorInsertDynamicInst(SPIRVValue *, SPIRVValue *, SPIRVValue *, SPIRVBasicBlock *) override;

  // Input functions
  friend std::istream &operator>>(std::istream &I, SPIRVModule &M);

private:
  SPIRVErrorLog ErrLog;
  SPIRVId NextId;
  SPIRVWord SPIRVVersion;
  unsigned short GeneratorId;
  unsigned short GeneratorVer;
  SPIRVInstructionSchemaKind InstSchema;
  SourceLanguage SrcLang;
  SPIRVWord SrcLangVer;
  std::vector<SPIRVId> SrcFiles;
  std::set<std::string> SrcExtension;
  std::set<std::string> SPIRVExt;
  SPIRVAddressingModelKind AddrModel;
  SPIRVMemoryModelKind MemoryModel;

  typedef std::map<SPIRVId, SPIRVEntry *> SPIRVIdToEntryMap;
  typedef std::vector<SPIRVEntry *> SPIRVEntryVector;
  typedef std::set<SPIRVId> SPIRVIdSet;
  typedef std::vector<SPIRVId> SPIRVIdVec;
  typedef std::vector<SPIRVFunction *> SPIRVFunctionVector;
  typedef std::vector<SPIRVTypeForwardPointer *> SPIRVForwardPointerVec;
  typedef std::vector<SPIRVType *> SPIRVTypeVec;
  typedef std::vector<SPIRVValue *> SPIRVConstantVector;
  typedef std::vector<SPIRVVariable *> SPIRVVariableVec;
  typedef std::vector<SPIRVString *> SPIRVStringVec;
  typedef std::vector<SPIRVMemberName *> SPIRVMemberNameVec;
  typedef std::vector<SPIRVDecorationGroup *> SPIRVDecGroupVec;
  typedef std::vector<SPIRVGroupDecorateGeneric *> SPIRVGroupDecVec;
  typedef std::vector<SPIRVEntryPoint *> SPIRVEnetryPointVec;
  typedef std::map<SPIRVId, SPIRVExtInstSetKind> SPIRVIdToBuiltinSetMap;
  typedef std::unordered_map<std::string, SPIRVString *> SPIRVStringMap;
  typedef std::map<SPIRVTypeStruct *, std::vector<std::pair<unsigned, SPIRVId>>> SPIRVUnknownStructFieldMap;

  SPIRVEntryVector ExecModeIdVec;
  SPIRVForwardPointerVec ForwardPointerVec;
  SPIRVTypeVec TypeVec;
  SPIRVIdToEntryMap IdEntryMap;
  SPIRVFunctionVector FuncVec;
  SPIRVConstantVector ConstVec;
  SPIRVVariableVec VariableVec;
  SPIRVEntryVector EntryNoId; // Entries without id
  SPIRVIdToBuiltinSetMap IdBuiltinMap;
  SPIRVIdSet NamedId;
  SPIRVStringVec StringVec;
  SPIRVMemberNameVec MemberNameVec;
  const SPIRVLine *CurrentLine;
  SPIRVDecorateSet DecorateSet;
  SPIRVDecGroupVec DecGroupVec;
  SPIRVGroupDecVec GroupDecVec;
  SPIRVEnetryPointVec EntryPointVec;
  SPIRVStringMap StrMap;
  SPIRVCapMap CapMap;
  SPIRVUnknownStructFieldMap UnknownStructFieldMap;
  std::map<unsigned, SPIRVTypeInt *> IntTypeMap;
  std::map<unsigned, SPIRVConstant *> LiteralMap;
  std::vector<SPIRVExtInst *> DebugInstVec;

  void layoutEntry(SPIRVEntry *Entry);
};

SPIRVModuleImpl::~SPIRVModuleImpl() {
  for (auto I : IdEntryMap)
    delete I.second;

  for (auto I : EntryNoId) {
    delete I;
  }

  for (auto C : CapMap)
    delete C.second;
}

const SPIRVLine *SPIRVModuleImpl::getCurrentLine() const {
  return CurrentLine;
}

void SPIRVModuleImpl::setCurrentLine(const SPIRVLine *Line) {
  CurrentLine = Line;
}

// Creates decoration group and group decorates from decorates shared by
// multiple targets.
void SPIRVModuleImpl::optimizeDecorates() {
  for (auto I = DecorateSet.begin(), E = DecorateSet.end(); I != E;) {
    auto D = *I;
    if (D->getOpCode() == OpMemberDecorate) {
      ++I;
      continue;
    }
    auto ER = DecorateSet.equal_range(D);
    if (std::distance(ER.first, ER.second) < 2) {
      I = ER.second;
      continue;
    }
    auto G = add(new SPIRVDecorationGroup(this, getId()));
    std::vector<SPIRVId> Targets;
    Targets.push_back(D->getTargetId());
    const_cast<SPIRVDecorateGeneric *>(D)->setTargetId(G->getId());
    G->getDecorations().insert(D);
    for (I = ER.first; I != ER.second; ++I) {
      auto E = *I;
      if (*E == *D)
        continue;
      Targets.push_back(E->getTargetId());
    }

    // WordCount is only 16 bits.  We can only have 65535 - FixedWC targets per
    // group.
    // For now, just skip using a group if the number of targets to too big
    if (Targets.size() < 65530) {
      DecorateSet.erase(ER.first, ER.second);
      auto GD = add(new SPIRVGroupDecorate(G, Targets));
      DecGroupVec.push_back(G);
      GroupDecVec.push_back(GD);
    }
  }
}

void SPIRVModuleImpl::postProcessExecutionModeId() {
  for (auto ExecModeId : ExecModeIdVec) {
    SPIRVExecutionModeId *E = static_cast<SPIRVExecutionModeId *>(ExecModeId);
    auto M = E->getExecutionMode();
    auto tid = E->getTargetId();
    auto ops = E->getOperands();
    SPIRVExecutionMode *execMode = nullptr;
    switch (M) {
    case ExecutionModeLocalSizeId: {
      execMode = add(new SPIRVExecutionMode(getEntry(tid), ExecutionModeLocalSizeId, ops[0], ops[1], ops[2]));
      break;
    }
    default:
      break;
    }
    if (execMode)
      static_cast<SPIRVFunction *>(getEntry(tid))->addExecutionMode(execMode);
  }
}

void SPIRVModuleImpl::addCapability(SPIRVCapabilityKind Cap) {
  addCapabilities(SPIRV::getCapability(Cap));
  if (hasCapability(Cap))
    return;

  CapMap.insert(std::make_pair(Cap, new SPIRVCapability(this, Cap)));
}

void SPIRVModuleImpl::addCapabilityInternal(SPIRVCapabilityKind Cap) {
  if (AutoAddCapability) {
    if (hasCapability(Cap))
      return;

    CapMap.insert(std::make_pair(Cap, new SPIRVCapability(this, Cap)));
  }
}

SPIRVConstant *SPIRVModuleImpl::getLiteralAsConstant(unsigned Literal) {
  auto Loc = LiteralMap.find(Literal);
  if (Loc != LiteralMap.end())
    return Loc->second;
  auto Ty = addIntegerType(32);
  auto V = new SPIRVConstant(this, Ty, getId(), static_cast<uint64_t>(Literal));
  LiteralMap[Literal] = V;
  addConstant(V);
  return V;
}

void SPIRVModuleImpl::layoutEntry(SPIRVEntry *E) {
  auto OC = E->getOpCode();
  switch (OC) {
  case OpString:
    addTo(StringVec, E);
    break;
  case OpMemberName:
    addTo(MemberNameVec, E);
    break;
  case OpVariable: {
    auto BV = static_cast<SPIRVVariable *>(E);
    if (!BV->getParent())
      addTo(VariableVec, E);
  } break;
  case OpExtInst: {
    SPIRVExtInst *EI = static_cast<SPIRVExtInst *>(E);
    if ((EI->getExtSetKind() == SPIRVEIS_Debug || EI->getExtSetKind() == SPIRVEIS_NonSemanticShaderDebugInfo100) &&
        EI->getExtOp() != SPIRVDebug::Declare && EI->getExtOp() != SPIRVDebug::Value &&
        EI->getExtOp() != SPIRVDebug::Scope && EI->getExtOp() != SPIRVDebug::NoScope) {
      DebugInstVec.push_back(EI);
    }
    break;
  }
  case OpExecutionModeId: {
    ExecModeIdVec.push_back(E);
    break;
  }
  default:
    if (isTypeOpCode(OC))
      TypeVec.push_back(static_cast<SPIRVType *>(E));
    else if (isConstantOpCode(OC))
      ConstVec.push_back(static_cast<SPIRVConstant *>(E));
    break;
  }
}

// Add an entry to the id to entry map.
// Assert if the id is mapped to a different entry.
// Certain entries need to be add to specific collectors to maintain
// logic layout of SPIRV.
SPIRVEntry *SPIRVModuleImpl::addEntry(SPIRVEntry *Entry) {
  assert(Entry && "Invalid entry");
  if (Entry->hasId()) {
    SPIRVId Id = Entry->getId();
    assert(Entry->getId() != SPIRVID_INVALID && "Invalid id");
    SPIRVEntry *Mapped = nullptr;
    if (exist(Id, &Mapped)) {
      if (Mapped->getOpCode() == OpForward) {
        replaceForward(static_cast<SPIRVForward *>(Mapped), Entry);
      } else if (Mapped->getOpCode() == OpTypeForwardPointer) {
        replaceForwardPointer(static_cast<SPIRVTypeForwardPointer *>(Mapped), static_cast<SPIRVTypePointer *>(Entry));
      } else {
        assert(Mapped == Entry && "Id used twice");
      }
    } else
      IdEntryMap[Id] = Entry;
  } else {
    if (EntryNoId.empty() || Entry != EntryNoId.back())
      EntryNoId.push_back(Entry);
  }

  Entry->setModule(this);

  layoutEntry(Entry);
  if (AutoAddCapability) {
    for (auto &I : Entry->getRequiredCapability()) {
      addCapability(I);
    }
  }
  if (ValidateCapability) {
    for (auto &I : Entry->getRequiredCapability()) {
      assert(CapMap.count(I));
      (void)I;
    }
  }
  return Entry;
}

bool SPIRVModuleImpl::exist(SPIRVId Id) const {
  return exist(Id, nullptr);
}

bool SPIRVModuleImpl::exist(SPIRVId Id, SPIRVEntry **Entry) const {
  assert(Id != SPIRVID_INVALID && "Invalid Id");
  SPIRVIdToEntryMap::const_iterator Loc = IdEntryMap.find(Id);
  if (Loc == IdEntryMap.end())
    return false;
  if (Entry)
    *Entry = Loc->second;
  return true;
}

// If Id is invalid, returns the next available id.
// Otherwise returns the given id and adjust the next available id by increment.
SPIRVId SPIRVModuleImpl::getId(SPIRVId Id, unsigned Increment) {
  if (!isValidId(Id))
    Id = NextId;
  else
    NextId = std::max(Id, NextId);
  NextId += Increment;
  return Id;
}

SPIRVEntry *SPIRVModuleImpl::getEntry(SPIRVId Id) const {
  assert(Id != SPIRVID_INVALID && "Invalid Id");
  SPIRVIdToEntryMap::const_iterator Loc = IdEntryMap.find(Id);
  assert(Loc != IdEntryMap.end() && "Id is not in map");
  return Loc->second;
}

SPIRVExtInstSetKind SPIRVModuleImpl::getBuiltinSet(SPIRVId SetId) const {
  auto Loc = IdBuiltinMap.find(SetId);
  assert(Loc != IdBuiltinMap.end() && "Invalid builtin set id");
  return Loc->second;
}

SPIRVEntryPoint *SPIRVModuleImpl::getEntryPoint(SPIRVId EP) const {
  assert(EP != SPIRVID_INVALID && "Invalid function id");
  for (auto EntryPoint : EntryPointVec) {
    if (EntryPoint->getTargetId() == EP)
      return EntryPoint;
  }
  return nullptr;
}

SPIRVEntryPoint *SPIRVModuleImpl::getEntryPoint(SPIRVExecutionModelKind ExecModel, const char *Name) const {
  for (auto EntryPoint : EntryPointVec) {
    if (EntryPoint->getExecModel() == ExecModel && EntryPoint->getName() == Name)
      return EntryPoint;
  }
  return nullptr;
}

bool SPIRVModuleImpl::isEntryPoint(SPIRVExecutionModelKind ExecModel, SPIRVId EP) const {
  assert(isValid(ExecModel) && "Invalid execution model");
  assert(EP != SPIRVID_INVALID && "Invalid function id");
  for (auto EntryPoint : EntryPointVec) {
    if (EntryPoint->getExecModel() == ExecModel && EntryPoint->getTargetId() == EP)
      return true;
  }
  return false;
}

// Module change functions
bool SPIRVModuleImpl::importBuiltinSet(const std::string &BuiltinSetName, SPIRVId *BuiltinSetId) {
  SPIRVId TmpBuiltinSetId = getId();
  if (!importBuiltinSetWithId(BuiltinSetName, TmpBuiltinSetId))
    return false;
  if (BuiltinSetId)
    *BuiltinSetId = TmpBuiltinSetId;
  return true;
}

bool SPIRVModuleImpl::isNonSemanticInfoInstSet(llvm::StringRef setName) const {
  return setName.startswith("NonSemantic.");
}

bool SPIRVModuleImpl::importBuiltinSetWithId(const std::string &BuiltinSetName, SPIRVId BuiltinSetId) {
  SPIRVExtInstSetKind BuiltinSet = SPIRVEIS_Count;

  if (SPIRVBuiltinSetNameMap::rfind(BuiltinSetName, &BuiltinSet))
    IdBuiltinMap[BuiltinSetId] = BuiltinSet;
  else if (isNonSemanticInfoInstSet(BuiltinSetName))
    IdBuiltinMap[BuiltinSetId] = SPIRVEIS_NonSemanticInfo;
  else
    SPIRVCKRT(false, InvalidBuiltinSetName, "Actual is " + BuiltinSetName);

  return true;
}

void SPIRVModuleImpl::setName(SPIRVEntry *E, const std::string &Name) {
  E->setName(Name);
  if (!E->hasId())
    return;
  if (!Name.empty())
    NamedId.insert(E->getId());
  else
    NamedId.erase(E->getId());
}

void SPIRVModuleImpl::resolveUnknownStructFields() {
  for (auto &KV : UnknownStructFieldMap) {
    auto *Struct = KV.first;
    for (auto &Indices : KV.second) {
      unsigned I = Indices.first;
      SPIRVId ID = Indices.second;

      auto Ty = static_cast<SPIRVType *>(getEntry(ID));
      Struct->setMemberType(I, Ty);
    }
  }
}

// Type creation functions
template <class T> T *SPIRVModuleImpl::addType(T *Ty) {
  add(Ty);
  if (!Ty->getName().empty())
    setName(Ty, Ty->getName());
  return Ty;
}

SPIRVTypeVoid *SPIRVModuleImpl::addVoidType() {
  return addType(new SPIRVTypeVoid(this, getId()));
}

SPIRVTypeArray *SPIRVModuleImpl::addArrayType(SPIRVType *ElementType, SPIRVConstant *Length) {
  return addType(new SPIRVTypeArray(this, getId(), ElementType, Length));
}

SPIRVTypeRuntimeArray *SPIRVModuleImpl::addRuntimeArray(SPIRVType *elementType) {
  return addType(new SPIRVTypeRuntimeArray(this, getId(), elementType));
}

SPIRVTypeStruct *SPIRVModuleImpl::addStructType(const std::vector<SPIRVType *> &vecTypes) {
  return addType(new SPIRVTypeStruct(this, getId(), vecTypes, ""));
}

SPIRVTypeBool *SPIRVModuleImpl::addBoolType() {
  return addType(new SPIRVTypeBool(this, getId()));
}

SPIRVTypeInt *SPIRVModuleImpl::addIntegerType(unsigned BitWidth) {
  auto Loc = IntTypeMap.find(BitWidth);
  if (Loc != IntTypeMap.end())
    return Loc->second;
  auto Ty = new SPIRVTypeInt(this, getId(), BitWidth, false);
  IntTypeMap[BitWidth] = Ty;
  return addType(Ty);
}

SPIRVTypeFloat *SPIRVModuleImpl::addFloatType(unsigned BitWidth) {
  SPIRVTypeFloat *T = addType(new SPIRVTypeFloat(this, getId(), BitWidth));
  return T;
}

SPIRVTypePointer *SPIRVModuleImpl::addPointerType(SPIRVStorageClassKind StorageClass, SPIRVType *ElementType) {
  return addType(new SPIRVTypePointer(this, getId(), StorageClass, ElementType));
}

SPIRVTypeFunction *SPIRVModuleImpl::addFunctionType(SPIRVType *ReturnType,
                                                    const std::vector<SPIRVType *> &ParameterTypes) {
  return addType(new SPIRVTypeFunction(this, getId(), ReturnType, ParameterTypes));
}

SPIRVTypeVector *SPIRVModuleImpl::addVectorType(SPIRVType *CompType, SPIRVWord CompCount) {
  return addType(new SPIRVTypeVector(this, getId(), CompType, CompCount));
}

SPIRVTypeImage *SPIRVModuleImpl::addImageType(SPIRVType *SampledType, const SPIRVTypeImageDescriptor &Desc) {
  return addType(new SPIRVTypeImage(this, getId(), SampledType ? SampledType->getId() : 0, Desc));
}

SPIRVTypeSampler *SPIRVModuleImpl::addSamplerType() {
  return addType(new SPIRVTypeSampler(this, getId()));
}

SPIRVTypeSampledImage *SPIRVModuleImpl::addSampledImageType(SPIRVTypeImage *T) {
  return addType(new SPIRVTypeSampledImage(this, getId(), T));
}

void SPIRVModuleImpl::createForwardPointers() {
  std::unordered_set<SPIRVId> Seen;

  for (auto *T : TypeVec) {
    if (T->hasId())
      Seen.insert(T->getId());

    if (!T->isTypeStruct())
      continue;

    auto ST = static_cast<SPIRVTypeStruct *>(T);

    for (unsigned I = 0; I < ST->getStructMemberCount(); ++I) {
      auto MemberTy = ST->getStructMemberType(I);
      if (!MemberTy->isTypePointer())
        continue;
      auto Ptr = static_cast<SPIRVTypePointer *>(MemberTy);

      if (Seen.find(Ptr->getId()) == Seen.end()) {
        ForwardPointerVec.push_back(new SPIRVTypeForwardPointer(this, Ptr, Ptr->getPointerStorageClass()));
      }
    }
  }
}

SPIRVFunction *SPIRVModuleImpl::addFunction(SPIRVFunction *Func) {
  FuncVec.push_back(add(Func));
  return Func;
}

SPIRVFunction *SPIRVModuleImpl::addFunction(SPIRVTypeFunction *FuncType, SPIRVId Id) {
  return addFunction(new SPIRVFunction(this, FuncType, getId(Id, FuncType->getNumParameters() + 1)));
}

SPIRVBasicBlock *SPIRVModuleImpl::addBasicBlock(SPIRVFunction *Func, SPIRVId Id) {
  return Func->addBasicBlock(new SPIRVBasicBlock(getId(Id), Func));
}

const SPIRVDecorateGeneric *SPIRVModuleImpl::addDecorate(const SPIRVDecorateGeneric *Dec) {
  SPIRVEntry *Target = nullptr;
  assert(exist(Dec->getTargetId(), &Target) && "Decorate target does not exist");
  (void)Target;
  if (!Dec->getOwner())
    DecorateSet.insert(Dec);
  addCapabilities(Dec->getRequiredCapability());
  return Dec;
}

void SPIRVModuleImpl::addEntryPoint(SPIRVEntryPoint *EntryPoint) {
  assert(EntryPoint != nullptr && "Invalid entry point");
  assert(isValid(EntryPoint->getExecModel()) && "Invalid execution model");
  EntryPointVec.push_back(EntryPoint);
  addCapabilities(SPIRV::getCapability(EntryPoint->getExecModel()));
}

SPIRVForward *SPIRVModuleImpl::addForward(SPIRVType *Ty) {
  return add(new SPIRVForward(this, Ty, getId()));
}

SPIRVForward *SPIRVModuleImpl::addForward(SPIRVId Id, SPIRVType *Ty) {
  return add(new SPIRVForward(this, Ty, Id));
}

SPIRVEntry *SPIRVModuleImpl::replaceForward(SPIRVForward *Forward, SPIRVEntry *Entry) {
  SPIRVId Id = Entry->getId();
  SPIRVId ForwardId = Forward->getId();
  if (ForwardId == Id)
    IdEntryMap[Id] = Entry;
  else {
    auto Loc = IdEntryMap.find(Id);
    assert(Loc != IdEntryMap.end());
    IdEntryMap.erase(Loc);
    Entry->setId(ForwardId);
    IdEntryMap[ForwardId] = Entry;
  }
  // Annotations include name, decorations, execution modes
  Entry->takeAnnotations(Forward);
  delete Forward;
  return Entry;
}

SPIRVEntry *SPIRVModuleImpl::replaceForwardPointer(SPIRVTypeForwardPointer *Forward, SPIRVTypePointer *Entry) {
  assert(Forward->getId() == Entry->getId());
  Forward->setPointer(Entry);
  return Forward;
}

void SPIRVModuleImpl::eraseInstruction(SPIRVInstruction *I, SPIRVBasicBlock *BB) {
  SPIRVId Id = I->getId();
  BB->eraseInstruction(I);
  auto Loc = IdEntryMap.find(Id);
  assert(Loc != IdEntryMap.end());
  IdEntryMap.erase(Loc);
  delete I;
}

SPIRVValue *SPIRVModuleImpl::addConstant(SPIRVValue *C) {
  return add(C);
}

SPIRVValue *SPIRVModuleImpl::addConstant(SPIRVType *Ty, uint64_t V) {
  if (Ty->isTypeBool()) {
    if (V)
      return addConstant(new SPIRVConstantTrue(this, Ty, getId()));
    return addConstant(new SPIRVConstantFalse(this, Ty, getId()));
  }
  if (Ty->isTypeInt())
    return addIntegerConstant(static_cast<SPIRVTypeInt *>(Ty), V);
  return addConstant(new SPIRVConstant(this, Ty, getId(), V));
}

SPIRVValue *SPIRVModuleImpl::addIntegerConstant(SPIRVTypeInt *Ty, uint64_t V) {
  if (Ty->getBitWidth() == 32) {
    unsigned I32 = static_cast<unsigned>(V);
    assert(I32 == V && "Integer value truncated");
    return getLiteralAsConstant(I32);
  }
  return addConstant(new SPIRVConstant(this, Ty, getId(), V));
}

SPIRVValue *SPIRVModuleImpl::addFloatConstant(SPIRVTypeFloat *Ty, float V) {
  return addConstant(new SPIRVConstant(this, Ty, getId(), V));
}

SPIRVValue *SPIRVModuleImpl::addDoubleConstant(SPIRVTypeFloat *Ty, double V) {
  return addConstant(new SPIRVConstant(this, Ty, getId(), V));
}

SPIRVValue *SPIRVModuleImpl::addNullConstant(SPIRVType *Ty) {
  return addConstant(new SPIRVConstantNull(this, Ty, getId()));
}

SPIRVValue *SPIRVModuleImpl::addCompositeConstant(SPIRVType *Ty, const std::vector<SPIRVValue *> &Elements) {
  return addConstant(new SPIRVConstantComposite(this, Ty, getId(), Elements));
}

SPIRVValue *SPIRVModuleImpl::addUndef(SPIRVType *TheType) {
  return addConstant(new SPIRVUndef(this, TheType, getId()));
}

// Instruction creation functions

SPIRVInstruction *SPIRVModuleImpl::addStoreInst(SPIRVValue *Target, SPIRVValue *Source,
                                                const std::vector<SPIRVWord> &TheMemoryAccess, SPIRVBasicBlock *BB) {
  return BB->addInstruction(new SPIRVStore(Target->getId(), Source->getId(), TheMemoryAccess, BB));
}

SPIRVInstruction *
SPIRVModuleImpl::addSwitchInst(SPIRVValue *Select, SPIRVBasicBlock *Default,
                               const std::vector<std::pair<std::vector<SPIRVWord>, SPIRVBasicBlock *>> &Pairs,
                               SPIRVBasicBlock *BB) {
  return BB->addInstruction(new SPIRVSwitch(Select, Default, Pairs, BB));
}
SPIRVInstruction *SPIRVModuleImpl::addFModInst(SPIRVType *TheType, SPIRVId TheDividend, SPIRVId TheDivisor,
                                               SPIRVBasicBlock *BB) {
  return BB->addInstruction(new SPIRVFMod(TheType, getId(), TheDividend, TheDivisor, BB));
}

SPIRVInstruction *SPIRVModuleImpl::addVectorTimesScalarInst(SPIRVType *TheType, SPIRVId TheVector, SPIRVId TheScalar,
                                                            SPIRVBasicBlock *BB) {
  return BB->addInstruction(new SPIRVVectorTimesScalar(TheType, getId(), TheVector, TheScalar, BB));
}

SPIRVInstruction *SPIRVModuleImpl::addGroupInst(Op OpCode, SPIRVType *Type, Scope Scope,
                                                const std::vector<SPIRVValue *> &Ops, SPIRVBasicBlock *BB) {
  assert(!Type || !Type->isTypeVoid());
  auto WordOps = getIds(Ops);
  WordOps.insert(WordOps.begin(), Scope);
  return addInstTemplate(OpCode, WordOps, BB, Type);
}

SPIRVInstruction *SPIRVModuleImpl::addInstruction(SPIRVInstruction *Inst, SPIRVBasicBlock *BB) {
  if (BB)
    return BB->addInstruction(Inst);
  if (Inst->getOpCode() != OpSpecConstantOp)
    Inst = createSpecConstantOpInst(Inst);
  return static_cast<SPIRVInstruction *>(addConstant(Inst));
}

SPIRVInstruction *SPIRVModuleImpl::addLoadInst(SPIRVValue *Source, const std::vector<SPIRVWord> &TheMemoryAccess,
                                               SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVLoad(getId(), Source->getId(), TheMemoryAccess, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addPhiInst(SPIRVType *Type, std::vector<SPIRVValue *> IncomingPairs,
                                              SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVPhi(Type, getId(), IncomingPairs, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addExtInst(SPIRVType *TheType, SPIRVWord BuiltinSet, SPIRVWord EntryPoint,
                                              const std::vector<SPIRVWord> &Args, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVExtInst(TheType, getId(), BuiltinSet, EntryPoint, Args, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addExtInst(SPIRVType *TheType, SPIRVWord BuiltinSet, SPIRVWord EntryPoint,
                                              const std::vector<SPIRVValue *> &Args, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVExtInst(TheType, getId(), BuiltinSet, EntryPoint, Args, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addCallInst(SPIRVFunction *TheFunction, const std::vector<SPIRVWord> &TheArguments,
                                               SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVFunctionCall(getId(), TheFunction, TheArguments, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addBinaryInst(Op TheOpCode, SPIRVType *Type, SPIRVValue *Op1, SPIRVValue *Op2,
                                                 SPIRVBasicBlock *BB) {
  return addInstruction(
      SPIRVInstTemplateBase::create(TheOpCode, Type, getId(), getVec(Op1->getId(), Op2->getId()), BB, this), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addUnreachableInst(SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVUnreachable(BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addReturnInst(SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVReturn(BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addReturnValueInst(SPIRVValue *ReturnValue, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVReturnValue(ReturnValue, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addUnaryInst(Op TheOpCode, SPIRVType *TheType, SPIRVValue *Op, SPIRVBasicBlock *BB) {
  return addInstruction(SPIRVInstTemplateBase::create(TheOpCode, TheType, getId(), getVec(Op->getId()), BB, this), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addVectorExtractDynamicInst(SPIRVValue *TheVector, SPIRVValue *Index,
                                                               SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVVectorExtractDynamic(getId(), TheVector, Index, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addVectorInsertDynamicInst(SPIRVValue *TheVector, SPIRVValue *TheComponent,
                                                              SPIRVValue *Index, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVVectorInsertDynamic(getId(), TheVector, TheComponent, Index, BB), BB);
}

SPIRVValue *SPIRVModuleImpl::addVectorShuffleInst(SPIRVType *Type, SPIRVValue *Vec1, SPIRVValue *Vec2,
                                                  const std::vector<SPIRVWord> &Components, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVVectorShuffle(getId(), Type, Vec1, Vec2, Components, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addBranchInst(SPIRVLabel *TargetLabel, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVBranch(TargetLabel, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addBranchConditionalInst(SPIRVValue *Condition, SPIRVLabel *TrueLabel,
                                                            SPIRVLabel *FalseLabel, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVBranchConditional(Condition, TrueLabel, FalseLabel, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addCmpInst(Op TheOpCode, SPIRVType *TheType, SPIRVValue *Op1, SPIRVValue *Op2,
                                              SPIRVBasicBlock *BB) {
  return addInstruction(
      SPIRVInstTemplateBase::create(TheOpCode, TheType, getId(), getVec(Op1->getId(), Op2->getId()), BB, this), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addControlBarrierInst(SPIRVValue *ExecKind, SPIRVValue *MemKind, SPIRVValue *MemSema,
                                                         SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVControlBarrier(ExecKind, MemKind, MemSema, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addMemoryBarrierInst(Scope ScopeKind, SPIRVWord MemFlag, SPIRVBasicBlock *BB) {
  return addInstruction(SPIRVInstTemplateBase::create(OpMemoryBarrier, nullptr, SPIRVID_INVALID,
                                                      getVec(static_cast<SPIRVWord>(ScopeKind), MemFlag), BB, this),
                        BB);
}

SPIRVInstruction *SPIRVModuleImpl::addSelectInst(SPIRVValue *Condition, SPIRVValue *Op1, SPIRVValue *Op2,
                                                 SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVSelect(getId(), Condition->getId(), Op1->getId(), Op2->getId(), BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addSelectionMergeInst(SPIRVId MergeBlock, SPIRVWord SelectionControl,
                                                         SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVSelectionMerge(MergeBlock, SelectionControl, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addLoopMergeInst(SPIRVId MergeBlock, SPIRVId ContinueTarget, SPIRVWord LoopControl,
                                                    std::vector<SPIRVWord> LoopControlParameters, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVLoopMerge(MergeBlock, ContinueTarget, LoopControl, LoopControlParameters, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addPtrAccessChainInst(SPIRVType *Type, SPIRVValue *Base,
                                                         std::vector<SPIRVValue *> Indices, SPIRVBasicBlock *BB,
                                                         bool IsInBounds) {
  return addInstruction(SPIRVInstTemplateBase::create(IsInBounds ? OpInBoundsPtrAccessChain : OpPtrAccessChain, Type,
                                                      getId(), getVec(Base->getId(), Base->getIds(Indices)), BB, this),
                        BB);
}

SPIRVInstruction *SPIRVModuleImpl::addCompositeConstructInst(SPIRVType *Type, const std::vector<SPIRVId> &Constituents,
                                                             SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVCompositeConstruct(Type, getId(), Constituents, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addCompositeExtractInst(SPIRVType *Type, SPIRVValue *TheVector,
                                                           const std::vector<SPIRVWord> &Indices, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVCompositeExtract(Type, getId(), TheVector, Indices, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addCompositeInsertInst(SPIRVValue *Object, SPIRVValue *Composite,
                                                          const std::vector<SPIRVWord> &Indices, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVCompositeInsert(getId(), Object, Composite, Indices, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addCopyObjectInst(SPIRVType *TheType, SPIRVValue *Operand, SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVCopyObject(TheType, getId(), Operand, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addCopyMemoryInst(SPIRVValue *TheTarget, SPIRVValue *TheSource,
                                                     const std::vector<SPIRVWord> &TheMemoryAccess,
                                                     SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVCopyMemory(TheTarget, TheSource, TheMemoryAccess, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addCopyMemorySizedInst(SPIRVValue *TheTarget, SPIRVValue *TheSource,
                                                          SPIRVValue *TheSize,
                                                          const std::vector<SPIRVWord> &TheMemoryAccess,
                                                          SPIRVBasicBlock *BB) {
  return addInstruction(new SPIRVCopyMemorySized(TheTarget, TheSource, TheSize, TheMemoryAccess, BB), BB);
}

SPIRVInstruction *SPIRVModuleImpl::addVariable(SPIRVType *Type, bool IsConstant, SPIRVLinkageTypeKind LinkageType,
                                               SPIRVValue *Initializer, const std::string &Name,
                                               SPIRVStorageClassKind StorageClass, SPIRVBasicBlock *BB) {
  SPIRVVariable *Variable = new SPIRVVariable(Type, getId(), Initializer, Name, StorageClass, BB, this);
  if (BB)
    return addInstruction(Variable, BB);

  add(Variable);
  if (LinkageType != LinkageTypeInternal)
    Variable->setLinkageType(LinkageType);
  return Variable;
}

template <class T, class B> spv_ostream &operator<<(spv_ostream &O, const std::multiset<T *, B> &V) {
  for (auto &I : V)
    O << *I;
  return O;
}

template <class T> void SPIRVModuleImpl::addTo(std::vector<T *> &V, SPIRVEntry *E) {
  V.push_back(static_cast<T *>(E));
}

// The first decoration group includes all the previously defined decorates.
// The second decoration group includes all the decorates defined between the
// first and second decoration group. So long so forth.
SPIRVDecorationGroup *SPIRVModuleImpl::addDecorationGroup() {
  return addDecorationGroup(new SPIRVDecorationGroup(this, getId()));
}

SPIRVDecorationGroup *SPIRVModuleImpl::addDecorationGroup(SPIRVDecorationGroup *Group) {
  add(Group);
  Group->takeDecorates(DecorateSet);
  DecGroupVec.push_back(Group);
  return Group;
}

SPIRVGroupDecorateGeneric *SPIRVModuleImpl::addGroupDecorateGeneric(SPIRVGroupDecorateGeneric *GDec) {
  add(GDec);
  GDec->decorateTargets();
  GroupDecVec.push_back(GDec);
  return GDec;
}
SPIRVGroupDecorate *SPIRVModuleImpl::addGroupDecorate(SPIRVDecorationGroup *Group,
                                                      const std::vector<SPIRVEntry *> &Targets) {
  auto GD = new SPIRVGroupDecorate(Group, getIds(Targets));
  addGroupDecorateGeneric(GD);
  return GD;
}

SPIRVGroupMemberDecorate *SPIRVModuleImpl::addGroupMemberDecorate(SPIRVDecorationGroup *Group,
                                                                  const std::vector<SPIRVEntry *> &Targets) {
  auto GMD = new SPIRVGroupMemberDecorate(Group, getIds(Targets));
  addGroupDecorateGeneric(GMD);
  return GMD;
}

SPIRVString *SPIRVModuleImpl::getString(const std::string &Str) {
  auto Loc = StrMap.find(Str);
  if (Loc != StrMap.end())
    return Loc->second;
  auto S = add(new SPIRVString(this, getId(), Str));
  StrMap[Str] = S;
  return S;
}

SPIRVMemberName *SPIRVModuleImpl::addMemberName(SPIRVTypeStruct *ST, SPIRVWord MemberNumber, const std::string &Name) {
  return add(new SPIRVMemberName(ST, MemberNumber, Name));
}

void SPIRVModuleImpl::addUnknownStructField(SPIRVTypeStruct *Struct, unsigned I, SPIRVId ID) {
  UnknownStructFieldMap[Struct].push_back(std::make_pair(I, ID));
}

std::istream &operator>>(std::istream &I, SPIRVModule &M) {
  SPIRVDecoder Decoder(I, M);
  SPIRVModuleImpl &MI = *static_cast<SPIRVModuleImpl *>(&M);
  // Disable automatic capability filling.
  MI.setAutoAddCapability(false);

  SPIRVWord Magic;
  Decoder >> Magic;
  assert(Magic == MagicNumber && "Invalid magic number");

  Decoder >> MI.SPIRVVersion;
  assert(MI.SPIRVVersion <= SPV_VERSION && "Unsupported SPIRV version number");

  SPIRVWord Generator = 0;
  Decoder >> Generator;
  MI.GeneratorId = Generator >> 16;
  MI.GeneratorVer = Generator & 0xFFFF;

  // Bound for Id
  Decoder >> MI.NextId;

  Decoder >> MI.InstSchema;
  assert(MI.InstSchema == SPIRVISCH_Default && "Unsupported instruction schema");

  while (Decoder.getWordCountAndOpCode())
    Decoder.getEntry();

  MI.postProcessExecutionModeId();
  MI.optimizeDecorates();
  MI.resolveUnknownStructFields();
  MI.createForwardPointers();
  return I;
}

SPIRVModule *SPIRVModule::createSPIRVModule() {
  return new SPIRVModuleImpl;
}

SPIRVValue *SPIRVModuleImpl::getValue(SPIRVId TheId) const {
  return get<SPIRVValue>(TheId);
}

SPIRVType *SPIRVModuleImpl::getValueType(SPIRVId TheId) const {
  return get<SPIRVValue>(TheId)->getType();
}

std::vector<SPIRVValue *> SPIRVModuleImpl::getValues(const std::vector<SPIRVId> &IdVec) const {
  std::vector<SPIRVValue *> ValueVec;
  ValueVec.reserve(IdVec.size());
  for (auto I : IdVec)
    ValueVec.push_back(getValue(I));
  return ValueVec;
}

std::vector<SPIRVType *> SPIRVModuleImpl::getValueTypes(const std::vector<SPIRVId> &IdVec) const {
  std::vector<SPIRVType *> TypeVec;
  TypeVec.reserve(IdVec.size());
  for (auto I : IdVec)
    TypeVec.push_back(getValue(I)->getType());
  return TypeVec;
}

std::vector<SPIRVId> SPIRVModuleImpl::getIds(const std::vector<SPIRVEntry *> &ValueVec) const {
  std::vector<SPIRVId> IdVec;
  IdVec.reserve(ValueVec.size());
  for (auto I : ValueVec)
    IdVec.push_back(I->getId());
  return IdVec;
}

std::vector<SPIRVId> SPIRVModuleImpl::getIds(const std::vector<SPIRVValue *> &ValueVec) const {
  std::vector<SPIRVId> IdVec;
  IdVec.reserve(ValueVec.size());
  for (auto I : ValueVec)
    IdVec.push_back(I->getId());
  return IdVec;
}

SPIRVInstTemplateBase *SPIRVModuleImpl::addInstTemplate(Op OC, SPIRVBasicBlock *BB, SPIRVType *Ty) {
  assert(!Ty || !Ty->isTypeVoid());
  SPIRVId Id = Ty ? getId() : SPIRVID_INVALID;
  auto Ins = SPIRVInstTemplateBase::create(OC, Ty, Id, BB, this);
  BB->addInstruction(Ins);
  return Ins;
}

SPIRVInstTemplateBase *SPIRVModuleImpl::addInstTemplate(Op OC, const std::vector<SPIRVWord> &Ops, SPIRVBasicBlock *BB,
                                                        SPIRVType *Ty) {
  assert(!Ty || !Ty->isTypeVoid());
  SPIRVId Id = Ty ? getId() : SPIRVID_INVALID;
  auto Ins = SPIRVInstTemplateBase::create(OC, Ty, Id, Ops, BB, this);
  BB->addInstruction(Ins);
  return Ins;
}

bool isSpirvBinary(const std::string &Img) {
  if (Img.size() < sizeof(unsigned))
    return false;
  auto Magic = reinterpret_cast<const unsigned *>(Img.data());
  return *Magic == MagicNumber;
}

} // namespace SPIRV
