//===- SPIRVReader.cpp - Converts SPIR-V to LLVM ----------------*- C++ -*-===//
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
/// This file implements conversion of SPIR-V binary to LLVM IR.
///
//===----------------------------------------------------------------------===//
#include "OCLUtil.h"
#include "SPIRVBasicBlock.h"
#include "SPIRVExtInst.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "SPIRVInternal.h"
#include "SPIRVMDBuilder.h"
#include "SPIRVModule.h"
#include "SPIRVType.h"
#include "SPIRVUtil.h"
#include "SPIRVValue.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>

#define DEBUG_TYPE "spirv"

using namespace std;
using namespace llvm;
using namespace SPIRV;
using namespace OCLUtil;

namespace SPIRV {

cl::opt<bool> SPIRVEnableStepExpansion(
    "spirv-expand-step", cl::init(true),
    cl::desc("Enable expansion of OpenCL step and smoothstep function"));

cl::opt<bool> SPIRVGenKernelArgNameMD(
    "spirv-gen-kernel-arg-name-md", cl::init(false),
    cl::desc("Enable generating OpenCL kernel argument name "
             "metadata"));

cl::opt<bool> SPIRVGenImgTypeAccQualPostfix(
    "spirv-gen-image-type-acc-postfix", cl::init(false),
    cl::desc("Enable generating access qualifier postfix"
             " in OpenCL image type names"));

cl::opt<bool> SPIRVGenFastMath("spirv-gen-fast-math",
    cl::init(true), cl::desc("Enable fast math mode with generating floating"
                              "point binary ops"));

cl::opt<bool> SPIRVWorkaroundBadSPIRV("spirv-workaround-bad-spirv",
    cl::init(true), cl::desc("Enable workarounds for bad SPIR-V"));

// Prefix for placeholder global variable name.
const char *KPlaceholderPrefix = "placeholder.";

// Save the translated LLVM before validation for debugging purpose.
static bool DbgSaveTmpLLVM = false;
static const char *DbgTmpLLVMFileName = "_tmp_llvmbil.ll";

namespace kOCLTypeQualifierName {
const static char *Const = "const";
const static char *Volatile = "volatile";
const static char *Restrict = "restrict";
const static char *Pipe = "pipe";
} // namespace kOCLTypeQualifierName

typedef std::pair<unsigned, AttributeList> AttributeWithIndex;

static bool isOpenCLKernel(SPIRVFunction *BF) {
  auto EntryPoint = BF->getModule()->getEntryPoint(BF->getId());
  return (EntryPoint != nullptr) &&
    (EntryPoint->getExecModel() == ExecutionModelKernel);
}

static void dumpLLVM(Module *M, const std::string &FName) {
  std::error_code EC;
  static int dumpIdx = 0;
  std::string UniqueFName = FName + "_" + std::to_string(dumpIdx++) + ".ll";
  raw_fd_ostream FS(UniqueFName, EC, sys::fs::F_None);
  if (!EC) {
    FS << *M;
    FS.close();
  }
}

static MDNode*
getMDNodeStringIntVec(LLVMContext *Context, const std::string& Str,
                      const std::vector<SPIRVWord>& IntVals) {
  std::vector<Metadata*> ValueVec;
  ValueVec.push_back(MDString::get(*Context, Str));
  for (auto &I:IntVals)
    ValueVec.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(*Context), I)));
  return MDNode::get(*Context, ValueVec);
}

static MDNode *getMDNodeStringIntVec(LLVMContext *Context,
                                     const std::vector<SPIRVWord> &IntVals) {
  std::vector<Metadata *> ValueVec;
  for (auto &I : IntVals)
    ValueVec.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(*Context), I)));
  return MDNode::get(*Context, ValueVec);
}

static MDNode *getMDTwoInt(LLVMContext *Context, unsigned Int1, unsigned Int2) {
  std::vector<Metadata *> ValueVec;
  ValueVec.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(*Context), Int1)));
  ValueVec.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(*Context), Int2)));
  return MDNode::get(*Context, ValueVec);
}

static MDNode*
getMDString(LLVMContext *Context, const std::string& Str) {
  std::vector<Metadata*> ValueVec;
  if (!Str.empty())
    ValueVec.push_back(MDString::get(*Context, Str));
  return MDNode::get(*Context, ValueVec);
}

static void addOCLVersionMetadata(LLVMContext *Context, Module *M,
                                  const std::string &MDName, unsigned Major,
                                  unsigned Minor) {
  NamedMDNode *NamedMD = M->getOrInsertNamedMetadata(MDName);
  NamedMD->addOperand(getMDTwoInt(Context, Major, Minor));
}

static void addNamedMetadataStringSet(LLVMContext *Context, Module *M,
                                      const std::string &MDName,
                                      const std::set<std::string> &StrSet) {
  NamedMDNode *NamedMD = M->getOrInsertNamedMetadata(MDName);
  std::vector<Metadata *> ValueVec;
  for (auto &&Str : StrSet) {
    ValueVec.push_back(MDString::get(*Context, Str));
  }
  NamedMD->addOperand(MDNode::get(*Context, ValueVec));
}

static void addOCLKernelArgumentMetadata(
    LLVMContext *Context, const std::string &MDName, SPIRVFunction *BF,
    llvm::Function *Fn,
    std::function<Metadata *(SPIRVFunctionParameter *)> Func) {
  std::vector<Metadata *> ValueVec;
  BF->foreachArgument(
      [&](SPIRVFunctionParameter *Arg) { ValueVec.push_back(Func(Arg)); });
  Fn->setMetadata(MDName, MDNode::get(*Context, ValueVec));
}

static void
mangleGlslBuiltin(const std::string &UniqName,
  ArrayRef<Type*> ArgTypes, std::string &MangledName) {
  BuiltinFuncMangleInfo Info(UniqName);
  MangledName = SPIRV::mangleBuiltin(UniqName, ArgTypes, &Info);
}

class SPIRVToLLVMDbgTran {
public:
  SPIRVToLLVMDbgTran(SPIRVModule *TBM, Module *TM)
      : BM(TBM), M(TM), SpDbg(BM), Builder(*M) {
    Enable = BM->hasDebugInfo();
  }

  void createCompileUnit() {
    if (!Enable)
      return;
    auto File = SpDbg.getEntryPointFileStr(ExecutionModelKernel, 0);
    if (File.empty())
      File = "spirv.dbg.cu"; // File name must be non-empty
    std::string BaseName;
    std::string Path;
    splitFileName(File, BaseName, Path);
    Builder.createCompileUnit(dwarf::DW_LANG_C99,
                              Builder.createFile(BaseName, Path), "spirv",
                              false, "", 0, "", DICompileUnit::LineTablesOnly);
  }

  void addDbgInfoVersion() {
    if (!Enable)
      return;
    M->addModuleFlag(Module::Warning, "Dwarf Version", dwarf::DWARF_VERSION);
    M->addModuleFlag(Module::Warning, "Debug Info Version",
                     DEBUG_METADATA_VERSION);
  }

  DIFile *getDIFile(const std::string &FileName) {
    return getOrInsert(FileMap, FileName, [=]() -> DIFile * {
      std::string BaseName;
      std::string Path;
      splitFileName(FileName, BaseName, Path);
      return Builder.createFile(BaseName, Path);
    });
  }

  DISubprogram *getDISubprogram(SPIRVFunction *SF, Function *F) {
    return getOrInsert(FuncMap, F, [=]() {
      auto DF = getDIFile(SpDbg.getFunctionFileStr(SF));
      auto FN = F->getName();
      auto LN = SpDbg.getFunctionLineNo(SF);
      return Builder.createFunction(
          DF, FN, FN, DF, LN,
          Builder.createSubroutineType(Builder.getOrCreateTypeArray(None)),
          Function::isInternalLinkage(F->getLinkage()), true, LN);
    });
  }

  void transDbgInfo(SPIRVValue *SV, Value *V) {
    if (!Enable || !SV->hasLine())
      return;
    if (auto I = dyn_cast<Instruction>(V)) {
      assert(SV->isInst() && "Invalid instruction");
      auto SI = static_cast<SPIRVInstruction *>(SV);
      assert(SI->getParent() && SI->getParent()->getParent() &&
             "Invalid instruction");
      auto Line = SV->getLine();
      I->setDebugLoc(
          DebugLoc::get(Line->getLine(), Line->getColumn(),
                        getDISubprogram(SI->getParent()->getParent(),
                                        I->getParent()->getParent())));
    }
  }

  void finalize() {
    if (!Enable)
      return;
    Builder.finalize();
  }

private:
  SPIRVModule *BM;
  Module *M;
  SPIRVDbgInfo SpDbg;
  DIBuilder Builder;
  bool Enable;
  std::unordered_map<std::string, DIFile *> FileMap;
  std::unordered_map<Function *, DISubprogram *> FuncMap;

  void splitFileName(const std::string &FileName, std::string &BaseName,
                     std::string &Path) {
    auto Loc = FileName.find_last_of("/\\");
    if (Loc != std::string::npos) {
      BaseName = FileName.substr(Loc + 1);
      Path = FileName.substr(0, Loc);
    } else {
      BaseName = FileName;
      Path = ".";
    }
  }
};

class SPIRVToLLVM {
public:
  SPIRVToLLVM(Module *LLVMModule, SPIRVModule *TheSPIRVModule,
    const SPIRVSpecConstMap &TheSpecConstMap)
    :M(LLVMModule), BM(TheSPIRVModule), IsKernel(true),
    EnableVarPtr(false), EntryTarget(nullptr),
    SpecConstMap(TheSpecConstMap), DbgTran(BM, M) {
    assert(M);
    Context = &M->getContext();
  }

  std::string getOCLBuiltinName(SPIRVInstruction *BI);
  std::string getOCLConvertBuiltinName(SPIRVInstruction *BI);
  std::string getOCLGenericCastToPtrName(SPIRVInstruction *BI);

  Type *transType(SPIRVType *BT, bool IsClassMember = false);
  std::string transTypeToOCLTypeName(SPIRVType *BT, bool IsSigned = true);
  std::vector<Type *> transTypeVector(const std::vector<SPIRVType *> &);
  bool translate(ExecutionModel EntryExecModel, const char *EntryName);
  bool transAddressingModel();

  Value *transValue(SPIRVValue *, Function *F, BasicBlock *,
                    bool CreatePlaceHolder = true);
  Value *transValueWithoutDecoration(SPIRVValue *, Function *F, BasicBlock *,
                                     bool CreatePlaceHolder = true);
  Value *transDeviceEvent(SPIRVValue *BV, Function *F, BasicBlock *BB);
  Value *transEnqueuedBlock(SPIRVValue *BF, SPIRVValue *BC, SPIRVValue *BCSize,
                            SPIRVValue *BCAligment, Function *F,
                            BasicBlock *BB);
  bool transDecoration(SPIRVValue *, Value *);
  bool transShaderDecoration(SPIRVValue *, Value *);
  bool transAlign(SPIRVValue *, Value *);
  Constant *buildShaderInOutMetadata(SPIRVType *BT, ShaderInOutDecorate &InOutDec, Type *&MetaTy);
  Constant *buildShaderBlockMetadata(SPIRVType* BT, ShaderBlockDecorate &BlockDec, Type *&MDTy);
  uint32_t calcShaderBlockSize(SPIRVType *BT, uint32_t BlockSize, uint32_t MatrixStride, bool IsRowMajor);
  Instruction *transOCLBuiltinFromExtInst(SPIRVExtInst *BC, BasicBlock *BB);
  Instruction *transGLSLBuiltinFromExtInst(SPIRVExtInst *BC, BasicBlock *BB);
  std::vector<Value *> transValue(const std::vector<SPIRVValue *> &,
                                  Function *F, BasicBlock *);
  Function *transFunction(SPIRVFunction *F);
  Instruction *transEnqueueKernelBI(SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transWGSizeBI(SPIRVInstruction *BI, BasicBlock *BB);
  bool transFPContractMetadata();
  bool transKernelMetadata();
  bool transNonTemporalMetadata(Instruction *I);
  bool transSourceLanguage();
  bool transSourceExtension();
  void transGeneratorMD();
  Value *transConvertInst(SPIRVValue *BV, Function *F, BasicBlock *BB);
  Instruction *transBuiltinFromInst(const std::string &FuncName,
                                    SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transOCLBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transSPIRVBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transOCLBarrierFence(SPIRVInstruction *BI, BasicBlock *BB);
  Value       *transSPIRVImageOpFromInst(SPIRVInstruction *BI, BasicBlock*BB);
  Instruction *transSPIRVFragmentMaskOpFromInst(SPIRVInstruction *BI, BasicBlock*BB);
  void transOCLVectorLoadStore(std::string &UnmangledName,
                               std::vector<SPIRVWord> &BArgs);

  /// Post-process translated LLVM module for OpenCL.
  bool postProcessOCL();

  /// \brief Post-process OpenCL builtin functions returning struct type.
  ///
  /// Some OpenCL builtin functions are translated to SPIR-V instructions with
  /// struct type result, e.g. NDRange creation functions. Such functions
  /// need to be post-processed to return the struct through sret argument.
  bool postProcessOCLBuiltinReturnStruct(Function *F);

  /// \brief Post-process OpenCL builtin functions having block argument.
  ///
  /// These functions are translated to functions with function pointer type
  /// argument first, then post-processed to have block argument.
  bool postProcessOCLBuiltinWithFuncPointer(Function *F,
      Function::arg_iterator I);

  /// \brief Post-process OpenCL builtin functions having array argument.
  ///
  /// These functions are translated to functions with array type argument
  /// first, then post-processed to have pointer arguments.
  bool
  postProcessOCLBuiltinWithArrayArguments(Function *F,
                                          const std::string &DemangledName);

  /// \brief Post-process OpImageSampleExplicitLod.
  ///   sampled_image = __spirv_SampledImage__(image, sampler);
  ///   return __spirv_ImageSampleExplicitLod__(sampled_image, image_operands,
  ///                                           ...);
  /// =>
  ///   read_image(image, sampler, ...)
  /// \return transformed call instruction.
  Instruction *postProcessOCLReadImage(SPIRVInstruction *BI, CallInst *CI,
                                       const std::string &DemangledName);

  /// \brief Post-process OpImageWrite.
  ///   return write_image(image, coord, color, image_operands, ...);
  /// =>
  ///   write_image(image, coord, ..., color)
  /// \return transformed call instruction.
  CallInst *postProcessOCLWriteImage(SPIRVInstruction *BI, CallInst *CI,
                                     const std::string &DemangledName);

  /// \brief Post-process OpBuildNDRange.
  ///   OpBuildNDRange GlobalWorkSize, LocalWorkSize, GlobalWorkOffset
  /// =>
  ///   call ndrange_XD(GlobalWorkOffset, GlobalWorkSize, LocalWorkSize)
  /// \return transformed call instruction.
  CallInst *postProcessOCLBuildNDRange(SPIRVInstruction *BI, CallInst *CI,
                                       const std::string &DemangledName);

  /// \brief Expand OCL builtin functions with scalar argument, e.g.
  /// step, smoothstep.
  /// gentype func (fp edge, gentype x)
  /// =>
  /// gentype func (gentype edge, gentype x)
  /// \return transformed call instruction.
  CallInst *expandOCLBuiltinWithScalarArg(CallInst *CI,
                                          const std::string &FuncName);

  /// \brief Post-process OpGroupAll and OpGroupAny instructions translation.
  /// i1 func (<n x i1> arg)
  /// =>
  /// i32 func (<n x i32> arg)
  /// \return transformed call instruction.
  Instruction *postProcessGroupAllAny(CallInst *CI,
                                      const std::string &DemangledName);

  typedef DenseMap<SPIRVType *, Type *> SPIRVToLLVMTypeMap;
  typedef DenseMap<SPIRVValue *, Value *> SPIRVToLLVMValueMap;
  typedef DenseMap<SPIRVValue *, Value *> SPIRVBlockToLLVMStructMap;
  typedef DenseMap<SPIRVFunction *, Function *> SPIRVToLLVMFunctionMap;
  typedef DenseMap<GlobalVariable *, SPIRVBuiltinVariableKind> BuiltinVarMap;

  // A SPIRV value may be translated to a load instruction of a placeholder
  // global variable. This map records load instruction of these placeholders
  // which are supposed to be replaced by the real values later.
  typedef std::map<SPIRVValue *, LoadInst *> SPIRVToLLVMPlaceholderMap;

private:
  Module *M;
  BuiltinVarMap BuiltinGVMap;
  LLVMContext *Context;
  SPIRVModule *BM;
  bool IsKernel;
  bool EnableVarPtr;
  bool EnableGatherLodNz;
  SPIRVFunction* EntryTarget;
  const SPIRVSpecConstMap &SpecConstMap;
  SPIRVToLLVMTypeMap TypeMap;
  SPIRVToLLVMValueMap ValueMap;
  SPIRVToLLVMFunctionMap FuncMap;
  SPIRVBlockToLLVMStructMap BlockMap;
  SPIRVToLLVMPlaceholderMap PlaceholderMap;
  SPIRVToLLVMDbgTran DbgTran;
  std::map<std::string, uint32_t> MangleNameToIndex;

  Type *mapType(SPIRVType *BT, Type *T) {
    SPIRVDBG(dbgs() << *T << '\n';)
    TypeMap[BT] = T;
    return T;
  }

  // If a value is mapped twice, the existing mapped value is a placeholder,
  // which must be a load instruction of a global variable whose name starts
  // with kPlaceholderPrefix.
  Value *mapValue(SPIRVValue *BV, Value *V) {
    auto Loc = ValueMap.find(BV);
    if (Loc != ValueMap.end()) {
      if (Loc->second == V)
        return V;
      auto LD = dyn_cast<LoadInst>(Loc->second);
      auto Placeholder = dyn_cast<GlobalVariable>(LD->getPointerOperand());
      assert(LD && Placeholder &&
             Placeholder->getName().startswith(KPlaceholderPrefix) &&
             "A value is translated twice");
      // Replaces placeholders for PHI nodes
      LD->replaceAllUsesWith(V);
      LD->eraseFromParent();
      Placeholder->eraseFromParent();
    }
    ValueMap[BV] = V;
    return V;
  }

  bool isSPIRVBuiltinVariable(GlobalVariable *GV,
                              SPIRVBuiltinVariableKind *Kind = nullptr) {
    auto Loc = BuiltinGVMap.find(GV);
    if (Loc == BuiltinGVMap.end())
      return false;
    if (Kind)
      *Kind = Loc->second;
    return true;
  }
  // OpenCL function always has NoUnwound attribute.
  // Change this if it is no longer true.
  bool isFuncNoUnwind() const { return true; }
  bool isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction *BI) const;
  bool transOCLBuiltinsFromVariables();
  bool transOCLBuiltinFromVariable(GlobalVariable *GV,
                                   SPIRVBuiltinVariableKind Kind);
  MDString *transOCLKernelArgTypeName(SPIRVFunctionParameter *);

  Value *mapFunction(SPIRVFunction *BF, Function *F) {
    SPIRVDBG(spvdbgs() << "[mapFunction] " << *BF << " -> ";
             dbgs() << *F << '\n';)
    FuncMap[BF] = F;
    return F;
  }

  Value *getTranslatedValue(SPIRVValue *BV);
  Type *getTranslatedType(SPIRVType *BT);
  IntrinsicInst *getLifetimeStartIntrinsic(Instruction *I);

  SPIRVErrorLog &getErrorLog() { return BM->getErrorLog(); }

  void setCallingConv(CallInst *Call) {
    Function *F = Call->getCalledFunction();
    assert(F);
    Call->setCallingConv(F->getCallingConv());
  }

  void setAttrByCalledFunc(CallInst *Call);
  Type *transFPType(SPIRVType *T);
  BinaryOperator *transShiftLogicalBitwiseInst(SPIRVValue *BV, BasicBlock *BB,
                                               Function *F);
  Instruction *transCmpInst(SPIRVValue *BV, BasicBlock *BB, Function *F);
  void transOCLBuiltinFromInstPreproc(SPIRVInstruction *BI,
                                      Type *&RetTy,
                                      std::vector<Type*> &ArgTys,
                                      std::vector<Value*> &Args,
                                      BasicBlock *BB);
  Instruction *transOCLBuiltinPostproc(SPIRVInstruction *BI, CallInst *CI,
                                       BasicBlock *BB,
                                       const std::string &DemangledName);
  std::string transOCLImageTypeName(SPIRV::SPIRVTypeImage *ST);
  std::string transGLSLImageTypeName(SPIRV::SPIRVTypeImage* ST);
  std::string transOCLSampledImageTypeName(SPIRV::SPIRVTypeSampledImage *ST);
  std::string transOCLPipeTypeName(SPIRV::SPIRVTypePipe *ST,
                                   bool UseSPIRVFriendlyFormat = false,
                                   int PipeAccess = 0);
  std::string transOCLPipeStorageTypeName(SPIRV::SPIRVTypePipeStorage *PST);
  std::string transOCLImageTypeAccessQualifier(SPIRV::SPIRVTypeImage *ST);
  std::string transOCLPipeTypeAccessQualifier(SPIRV::SPIRVTypePipe *ST);

  Value *oclTransConstantSampler(SPIRV::SPIRVConstantSampler *BCS);
  Value *oclTransConstantPipeStorage(SPIRV::SPIRVConstantPipeStorage *BCPS);
  void setName(llvm::Value *V, SPIRVValue *BV);
  void setLLVMLoopMetadata(SPIRVLoopMerge *LM, BranchInst *BI);
  void insertImageNameAccessQualifier(SPIRV::SPIRVTypeImage *ST,
                                      std::string &Name);
  template <class Source, class Func> bool foreachFuncCtlMask(Source, Func);
  llvm::GlobalValue::LinkageTypes transLinkageType(const SPIRVValue *V);
  Instruction *transOCLAllAny(SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transOCLRelational(SPIRVInstruction *BI, BasicBlock *BB);

  CallInst *transOCLBarrier(BasicBlock *BB, SPIRVWord ExecScope,
                            SPIRVWord MemSema, SPIRVWord MemScope);

  CallInst *transOCLMemFence(BasicBlock *BB, SPIRVWord MemSema,
                             SPIRVWord MemScope);
  void truncConstantIndex(std::vector<Value*> &Indices, BasicBlock *BB);

  Type *widenBoolType(Type *Ty);
  Value *widenBoolValue(Value *V, BasicBlock *BB);
  Constant *widenBoolConstant(Constant *C);
  Value *narrowBoolValue(Value *V, SPIRVType *BT, BasicBlock *BB);
};

Type *SPIRVToLLVM::getTranslatedType(SPIRVType *BV) {
  auto Loc = TypeMap.find(BV);
  if (Loc != TypeMap.end())
    return Loc->second;
  return nullptr;
}

Value *SPIRVToLLVM::getTranslatedValue(SPIRVValue *BV) {
  auto Loc = ValueMap.find(BV);
  if (Loc != ValueMap.end())
    return Loc->second;
  return nullptr;
}

IntrinsicInst *SPIRVToLLVM::getLifetimeStartIntrinsic(Instruction *I) {
  auto II = dyn_cast<IntrinsicInst>(I);
  if (II && II->getIntrinsicID() == Intrinsic::lifetime_start)
    return II;
  // Bitcast might be inserted during translation of OpLifetimeStart
  auto BC = dyn_cast<BitCastInst>(I);
  if (BC) {
    for (const auto &U : BC->users()) {
      II = dyn_cast<IntrinsicInst>(U);
      if (II && II->getIntrinsicID() == Intrinsic::lifetime_start)
        return II;
      ;
    }
  }
  return nullptr;
}

void SPIRVToLLVM::setAttrByCalledFunc(CallInst *Call) {
  Function *F = Call->getCalledFunction();
  assert(F);
  if (F->isIntrinsic()) {
    return;
  }
  Call->setCallingConv(F->getCallingConv());
  Call->setAttributes(F->getAttributes());
}

bool SPIRVToLLVM::transOCLBuiltinsFromVariables() {
  std::vector<GlobalVariable *> WorkList;
  for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I) {
    SPIRVBuiltinVariableKind Kind;
    if (!isSPIRVBuiltinVariable(&(*I), &Kind))
      continue;
    if (!transOCLBuiltinFromVariable(&(*I), Kind))
      return false;
    WorkList.push_back(&(*I));
  }
  for (auto &I : WorkList) {
    I->eraseFromParent();
  }
  return true;
}

// For integer types shorter than 32 bit, unsigned/signedness can be inferred
// from zext/sext attribute.
MDString *SPIRVToLLVM::transOCLKernelArgTypeName(SPIRVFunctionParameter *Arg) {
  auto Ty =
      Arg->isByVal() ? Arg->getType()->getPointerElementType() : Arg->getType();
  return MDString::get(*Context, transTypeToOCLTypeName(Ty, !Arg->isZext()));
}

// Variable like GlobalInvolcationId[x] -> get_global_id(x).
// Variable like WorkDim -> get_work_dim().
bool SPIRVToLLVM::transOCLBuiltinFromVariable(GlobalVariable *GV,
                                              SPIRVBuiltinVariableKind Kind) {
  std::string FuncName = SPIRSPIRVBuiltinVariableMap::rmap(Kind);
  std::string MangledName;
  Type *ReturnTy = GV->getType()->getPointerElementType();
  bool IsVec = ReturnTy->isVectorTy();
  if (!IsKernel) {
    // TODO: Built-ins with vector types can be used directly in GLSL without
    // additional operations. We replaced their import and export with function
    // call. Extra operations might be needed for array types.
    IsVec = false;
  }
  if (IsVec)
    ReturnTy = cast<VectorType>(ReturnTy)->getElementType();
  std::vector<Type *> ArgTy;
  if (IsVec)
    ArgTy.push_back(Type::getInt32Ty(*Context));
  mangleOpenClBuiltin(FuncName, ArgTy, MangledName);
  Function *Func = M->getFunction(MangledName);
  if (!Func) {
    FunctionType *FT = FunctionType::get(ReturnTy, ArgTy, false);
    Func = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    Func->addFnAttr(Attribute::NoUnwind);
    Func->addFnAttr(Attribute::ReadNone);
  }
  std::vector<Instruction *> Deletes;
  std::vector<Instruction *> Uses;
  for (auto UI = GV->user_begin(), UE = GV->user_end(); UI != UE; ++UI) {
    assert(isa<LoadInst>(*UI) && "Unsupported use");
    auto LD = dyn_cast<LoadInst>(*UI);
    if (!IsVec) {
      Uses.push_back(LD);
      Deletes.push_back(LD);
      continue;
    }
    for (auto LDUI = LD->user_begin(), LDUE = LD->user_end(); LDUI != LDUE;
         ++LDUI) {
      assert(isa<ExtractElementInst>(*LDUI) && "Unsupported use");
      auto EEI = dyn_cast<ExtractElementInst>(*LDUI);
      Uses.push_back(EEI);
      Deletes.push_back(EEI);
    }
    Deletes.push_back(LD);
  }
  for (auto &I : Uses) {
    std::vector<Value *> Arg;
    if (auto EEI = dyn_cast<ExtractElementInst>(I))
      Arg.push_back(EEI->getIndexOperand());
    auto Call = CallInst::Create(Func, Arg, "", I);
    Call->takeName(I);
    setAttrByCalledFunc(Call);
    SPIRVDBG(dbgs() << "[transOCLBuiltinFromVariable] " << *I << " -> " << *Call
                    << '\n';)
    I->replaceAllUsesWith(Call);
  }
  for (auto &I : Deletes) {
    I->eraseFromParent();
  }
  return true;
}

Type *SPIRVToLLVM::transFPType(SPIRVType *T) {
  switch (T->getFloatBitWidth()) {
  case 16:
    return Type::getHalfTy(*Context);
  case 32:
    return Type::getFloatTy(*Context);
  case 64:
    return Type::getDoubleTy(*Context);
  default:
    llvm_unreachable("Invalid type");
    return nullptr;
  }
}

std::string SPIRVToLLVM::transOCLImageTypeName(SPIRV::SPIRVTypeImage *ST) {
  std::string Name = std::string(kSPR2TypeName::OCLPrefix) +
                     rmap<std::string>(ST->getDescriptor());
  if (SPIRVGenImgTypeAccQualPostfix)
    SPIRVToLLVM::insertImageNameAccessQualifier(ST, Name);
  return Name;
}

std::string SPIRVToLLVM::transGLSLImageTypeName(SPIRV::SPIRVTypeImage* ST) {
  return getSPIRVTypeName(kSPIRVTypeName::SampledImg,
    getSPIRVImageTypePostfixes(getSPIRVImageSampledTypeName(
      ST->getSampledType()),
      ST->getDescriptor(),
      ST->getAccessQualifier()));
}

std::string
SPIRVToLLVM::transOCLSampledImageTypeName(SPIRV::SPIRVTypeSampledImage* ST) {
  return getSPIRVTypeName(kSPIRVTypeName::SampledImg,
    getSPIRVImageTypePostfixes(getSPIRVImageSampledTypeName(
      ST->getImageType()->getSampledType()),
      ST->getImageType()->getDescriptor(),
      ST->getImageType()->getAccessQualifier()));
}

std::string SPIRVToLLVM::transOCLPipeTypeName(SPIRV::SPIRVTypePipe *PT,
                                              bool UseSPIRVFriendlyFormat,
                                              int PipeAccess) {
  if (!UseSPIRVFriendlyFormat)
    return kSPR2TypeName::Pipe;
  else
    return std::string(kSPIRVTypeName::PrefixAndDelim) + kSPIRVTypeName::Pipe +
           kSPIRVTypeName::Delimiter + kSPIRVTypeName::PostfixDelim +
           PipeAccess;
}

std::string
SPIRVToLLVM::transOCLPipeStorageTypeName(SPIRV::SPIRVTypePipeStorage *PST) {
  return std::string(kSPIRVTypeName::PrefixAndDelim) +
         kSPIRVTypeName::PipeStorage;
}

Type *SPIRVToLLVM::transType(SPIRVType *T, bool IsClassMember) {
  auto Loc = TypeMap.find(T);
  if (Loc != TypeMap.end())
    return Loc->second;

  SPIRVDBG(spvdbgs() << "[transType] " << *T << " -> ";)
  T->validate();
  switch (T->getOpCode()) {
  case OpTypeVoid:
    return mapType(T, Type::getVoidTy(*Context));
  case OpTypeBool:
    return mapType(T, Type::getInt1Ty(*Context));
  case OpTypeInt:
    return mapType(T, Type::getIntNTy(*Context, T->getIntegerBitWidth()));
  case OpTypeFloat:
    return mapType(T, transFPType(T));
  case OpTypeArray:
    return mapType(T, ArrayType::get(
        widenBoolType(transType(T->getArrayElementType())),
        T->getArrayLength()));
  case OpTypeRuntimeArray:
    return mapType(T, ArrayType::get(widenBoolType(
            transType(T->getArrayElementType())),
        SPIRVWORD_MAX));
  case OpTypePointer:
      if ((T->getPointerStorageClass() == StorageClassStorageBuffer) &&
        EnableVarPtr) {
        // NOTE: Pointer to storage buffer will be converted to this structure <descriptor, offset>.
        assert(DescriptorSizeBuffer == 4);
        auto Vec4Ty = VectorType::get(Type::getInt32Ty(*Context), DescriptorSizeBuffer);
        SmallVector<Type *, 4> MT;
        MT.push_back(Vec4Ty);
        MT.push_back(Type::getInt32Ty(*Context));
        return mapType(T, StructType::get(*Context, MT));
    } else
      return mapType(T, PointerType::get(widenBoolType(transType(
        T->getPointerElementType(), IsClassMember)),
               SPIRSPIRVAddrSpaceMap::rmap(T->getPointerStorageClass())));
  case OpTypeVector:
    return mapType(T, VectorType::get(transType(T->getVectorComponentType()),
                                      T->getVectorComponentCount()));
  case OpTypeMatrix:
    return mapType(T, ArrayType::get(widenBoolType(
            transType(T->getMatrixColumnType())),
        T->getMatrixColumnCount()));
  case OpTypeOpaque:
    return mapType(T, StructType::create(*Context, T->getName()));
  case OpTypeFunction: {
    auto FT = static_cast<SPIRVTypeFunction *>(T);
    auto RT = transType(FT->getReturnType());
    std::vector<Type *> PT;
    for (size_t I = 0, E = FT->getNumParameters(); I != E; ++I)
      PT.push_back(transType(FT->getParameterType(I)));
    return mapType(T, FunctionType::get(RT, PT, false));
  }
  case OpTypeImage: {
    auto ST = static_cast<SPIRVTypeImage *>(T);
    if (ST->isOCLImage())
      return mapType(T, getOrCreateOpaquePtrType(M, transOCLImageTypeName(ST)));
    else
      return mapType(T, getOrCreateOpaquePtrType(M,
          transGLSLImageTypeName(ST)));
    return nullptr;
  }
  case OpTypeSampler:
    return mapType(T, Type::getInt32Ty(*Context));
  case OpTypeSampledImage: {
    auto ST = static_cast<SPIRVTypeSampledImage *>(T);
    return mapType(
        T, getOrCreateOpaquePtrType(M, transOCLSampledImageTypeName(ST)));
  }
  case OpTypeStruct: {
    auto ST = static_cast<SPIRVTypeStruct *>(T);
    auto Name = ST->getName();
    if (!Name.empty()) {
      if (auto OldST = M->getTypeByName(Name))
        OldST->setName("");
    }
    SmallVector<Type *, 4> MT;
    for (size_t I = 0, E = ST->getMemberCount(); I != E; ++I)
      MT.push_back(widenBoolType(transType(ST->getMemberType(I), true)));

    StructType *StructTy = nullptr;
    if (ST->isLiteral())
      StructTy = StructType::get(*Context, MT, ST->isPacked());
    else {
      StructTy = StructType::create(*Context, Name);
      StructTy->setBody(MT, ST->isPacked());
    }
    mapType(ST, StructTy);
    return StructTy;
  }
  case OpTypePipe: {
    auto PT = static_cast<SPIRVTypePipe *>(T);
    return mapType(T, getOrCreateOpaquePtrType(
                          M,
                          transOCLPipeTypeName(PT, IsClassMember,
                                               PT->getAccessQualifier()),
                          getOCLOpaqueTypeAddrSpace(T->getOpCode())));
  }
  case OpTypePipeStorage: {
    auto PST = static_cast<SPIRVTypePipeStorage *>(T);
    return mapType(
        T, getOrCreateOpaquePtrType(M, transOCLPipeStorageTypeName(PST),
                                    getOCLOpaqueTypeAddrSpace(T->getOpCode())));
  }
  default: {
    auto OC = T->getOpCode();
    if (isOpaqueGenericTypeOpCode(OC))
      return mapType(
          T, getOrCreateOpaquePtrType(M, OCLOpaqueTypeOpCodeMap::rmap(OC),
                                      getOCLOpaqueTypeAddrSpace(OC)));
    llvm_unreachable("Not implemented");
  }
  }
  return 0;
}

std::string SPIRVToLLVM::transTypeToOCLTypeName(SPIRVType *T, bool IsSigned) {
  switch (T->getOpCode()) {
  case OpTypeVoid:
    return "void";
  case OpTypeBool:
    return "bool";
  case OpTypeInt: {
    std::string Prefix = IsSigned ? "" : "u";
    switch (T->getIntegerBitWidth()) {
    case 8:
      return Prefix + "char";
    case 16:
      return Prefix + "short";
    case 32:
      return Prefix + "int";
    case 64:
      return Prefix + "long";
    default:
      llvm_unreachable("invalid integer size");
      return Prefix + std::string("int") + T->getIntegerBitWidth() + "_t";
    }
  } break;
  case OpTypeFloat:
    switch (T->getFloatBitWidth()) {
    case 16:
      return "half";
    case 32:
      return "float";
    case 64:
      return "double";
    default:
      llvm_unreachable("invalid floating pointer bitwidth");
      return std::string("float") + T->getFloatBitWidth() + "_t";
    }
    break;
  case OpTypeArray:
    return "array";
  case OpTypePointer:
    return transTypeToOCLTypeName(T->getPointerElementType()) + "*";
  case OpTypeVector:
    return transTypeToOCLTypeName(T->getVectorComponentType()) +
           T->getVectorComponentCount();
  case OpTypeOpaque:
    return T->getName();
  case OpTypeFunction:
    llvm_unreachable("Unsupported");
    return "function";
  case OpTypeStruct: {
    auto Name = T->getName();
    if (Name.find("struct.") == 0)
      Name[6] = ' ';
    else if (Name.find("union.") == 0)
      Name[5] = ' ';
    return Name;
  }
  case OpTypePipe:
    return "pipe";
  case OpTypeSampler:
    return "sampler_t";
  case OpTypeImage: {
    std::string Name;
    Name = rmap<std::string>(static_cast<SPIRVTypeImage *>(T)->getDescriptor());
    if (SPIRVGenImgTypeAccQualPostfix) {
      auto ST = static_cast<SPIRVTypeImage *>(T);
      insertImageNameAccessQualifier(ST, Name);
    }
    return Name;
  }
  default:
    if (isOpaqueGenericTypeOpCode(T->getOpCode())) {
      return OCLOpaqueTypeOpCodeMap::rmap(T->getOpCode());
    }
    llvm_unreachable("Not implemented");
    return "unknown";
  }
}

std::vector<Type *>
SPIRVToLLVM::transTypeVector(const std::vector<SPIRVType *> &BT) {
  std::vector<Type *> T;
  for (auto I : BT)
    T.push_back(transType(I));
  return T;
}

std::vector<Value *>
SPIRVToLLVM::transValue(const std::vector<SPIRVValue *> &BV, Function *F,
                        BasicBlock *BB) {
  std::vector<Value *> V;
  for (auto I : BV)
    V.push_back(transValue(I, F, BB));
  return V;
}

bool SPIRVToLLVM::isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction *BI) const {
  auto OC = BI->getOpCode();
  return isCmpOpCode(OC) && !(OC >= OpLessOrGreater && OC <= OpUnordered);
}

void SPIRVToLLVM::setName(llvm::Value *V, SPIRVValue *BV) {
  auto Name = BV->getName();
  if (!Name.empty() && (!V->hasName() || Name != V->getName()))
    V->setName(Name);
}

void SPIRVToLLVM::setLLVMLoopMetadata(SPIRVLoopMerge *LM, BranchInst *BI) {
  if (!LM)
    return;
  llvm::MDString *Name = nullptr;
  auto Temp = MDNode::getTemporary(*Context, None);
  auto Self = MDNode::get(*Context, Temp.get());
  Self->replaceOperandWith(0, Self);

  if (LM->getLoopControl() == LoopControlMaskNone) {
    BI->setMetadata("llvm.loop", Self);
    return;
  } else if (LM->getLoopControl() == LoopControlUnrollMask)
    Name = llvm::MDString::get(*Context, "llvm.loop.unroll.full");
  else if (LM->getLoopControl() == LoopControlDontUnrollMask)
    Name = llvm::MDString::get(*Context, "llvm.loop.unroll.disable");
  else
    return;

  std::vector<llvm::Metadata *> OpValues(1, Name);
  SmallVector<llvm::Metadata *, 2> Metadata;
  Metadata.push_back(llvm::MDNode::get(*Context, Self));
  Metadata.push_back(llvm::MDNode::get(*Context, OpValues));

  llvm::MDNode *Node = llvm::MDNode::get(*Context, Metadata);
  Node->replaceOperandWith(0, Node);
  BI->setMetadata("llvm.loop", Node);
}

void SPIRVToLLVM::insertImageNameAccessQualifier(SPIRV::SPIRVTypeImage *ST,
                                                 std::string &Name) {
  std::string QName = rmap<std::string>(ST->getAccessQualifier());
  // transform: read_only -> ro, write_only -> wo, read_write -> rw
  QName = QName.substr(0, 1) + QName.substr(QName.find("_") + 1, 1) + "_";
  assert(!Name.empty() && "image name should not be empty");
  Name.insert(Name.size() - 1, QName);
}

Value *SPIRVToLLVM::transValue(SPIRVValue *BV, Function *F, BasicBlock *BB,
                               bool CreatePlaceHolder) {
  SPIRVToLLVMValueMap::iterator Loc = ValueMap.find(BV);

  // Replace storage buffer variable with the emulation getter call
  if (EnableVarPtr && (BV->getOpCode() == OpVariable) &&
    ((static_cast<SPIRVVariable*>(BV))->getStorageClass()
      == StorageClassStorageBuffer) &&
    (BB != nullptr) && (Loc != ValueMap.end())){
    Value* GV = Loc->second;
    auto ArgTys = { GV->getType() };
    auto RetTy = transType(BV->getType(), false);
    std::string MangledName = "";
    uint32_t MangleIdx = 0;
    mangleGlslBuiltin(gSPIRVMD::StorageBufferCall, ArgTys, MangledName);

    // Replace complex mangle name with simple mangle index ext
    if (MangleNameToIndex.find(MangledName) == MangleNameToIndex.end()) {
      MangleIdx = MangleNameToIndex.size();
      MangleNameToIndex[MangledName] = MangleIdx;
    }
    else
      MangleIdx = MangleNameToIndex[MangledName];

    MangledName = gSPIRVMD::StorageBufferCall + std::to_string(MangleIdx);
    auto NewF = getOrCreateFunction(M, RetTy, ArgTys, MangledName);
    BasicBlock* EntryBB = &*BB->getParent()->begin();
    Value* NewCall = nullptr;
    if (EntryBB->size() > 0) {
      auto InsertPt = EntryBB->getFirstInsertionPt();
      NewCall = CallInst::Create(NewF, { GV }, "", &*InsertPt);
    } else
      // Empty Block
      NewCall = CallInst::Create(NewF, { GV }, "", EntryBB);

    return NewCall;
  }

  if (Loc != ValueMap.end() && (!PlaceholderMap.count(BV) || CreatePlaceHolder))
    return Loc->second;

  SPIRVDBG(spvdbgs() << "[transValue] " << *BV << " -> ";)
  BV->validate();

  auto V = transValueWithoutDecoration(BV, F, BB, CreatePlaceHolder);
  if (!V) {
    SPIRVDBG(dbgs() << " Warning ! nullptr\n";)
    return nullptr;
  }
  setName(V, BV);
  if (!transDecoration(BV, V)) {
    assert(0 && "trans decoration fail");
    return nullptr;
  }

  SPIRVDBG(dbgs() << *V << '\n';)

  return V;
}

Value *SPIRVToLLVM::transDeviceEvent(SPIRVValue *BV, Function *F,
                                     BasicBlock *BB) {
  auto Val = transValue(BV, F, BB, false);
  auto Ty = dyn_cast<PointerType>(Val->getType());
  assert(Ty && "Invalid Device Event");
  if (Ty->getAddressSpace() == SPIRAS_Generic)
    return Val;

  IRBuilder<> Builder(BB);
  auto EventTy = PointerType::get(Ty->getElementType(), SPIRAS_Generic);
  return Builder.CreateAddrSpaceCast(Val, EventTy);
}

Value *SPIRVToLLVM::transConvertInst(SPIRVValue *BV, Function *F,
                                     BasicBlock *BB) {
  SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
  auto Src = transValue(BC->getOperand(0), F, BB, BB ? true : false);
  auto Dst = transType(BC->getType());
  CastInst::CastOps CO = Instruction::BitCast;
  bool IsExt =
      Dst->getScalarSizeInBits() > Src->getType()->getScalarSizeInBits();
  switch (BC->getOpCode()) {
  case OpPtrCastToGeneric:
  case OpGenericCastToPtr:
    CO = Instruction::AddrSpaceCast;
    break;
  case OpSConvert:
    CO = IsExt ? Instruction::SExt : Instruction::Trunc;
    break;
  case OpUConvert:
    CO = IsExt ? Instruction::ZExt : Instruction::Trunc;
    break;
  case OpFConvert:
    CO = IsExt ? Instruction::FPExt : Instruction::FPTrunc;
    break;
  default:
    CO = static_cast<CastInst::CastOps>(OpCodeMap::rmap(BC->getOpCode()));
  }

  if (Dst == Src->getType())
    return Src;
  else {
    assert(CastInst::isCast(CO) && "Invalid cast op code");
    SPIRVDBG(if (!CastInst::castIsValid(CO, Src, Dst)) {
      spvdbgs() << "Invalid cast: " << *BV << " -> ";
      dbgs() << "Op = " << CO << ", Src = " << *Src << " Dst = " << *Dst << '\n';
    })
    if (BB)
      return CastInst::Create(CO, Src, Dst, BV->getName(), BB);
    return ConstantExpr::getCast(CO, dyn_cast<Constant>(Src), Dst);
  }
}

BinaryOperator *SPIRVToLLVM::transShiftLogicalBitwiseInst(SPIRVValue *BV,
                                                          BasicBlock *BB,
                                                          Function *F) {
  SPIRVBinary *BBN = static_cast<SPIRVBinary *>(BV);
  assert(BB && "Invalid BB");
  Instruction::BinaryOps BO;
  auto OP = BBN->getOpCode();
  if (isLogicalOpCode(OP))
    OP = IntBoolOpMap::rmap(OP);
  BO = static_cast<Instruction::BinaryOps>(OpCodeMap::rmap(OP));
  auto Base = transValue(BBN->getOperand(0), F, BB);
  auto Shift = transValue(BBN->getOperand(1), F, BB);

  // NOTE: SPIR-V spec allows the operands "base" and "shift" to have different
  // bit width.
  auto BaseBitWidth = Base->getType()->getScalarSizeInBits();
  auto ShiftBitWidth = Shift->getType()->getScalarSizeInBits();
  if (BaseBitWidth != ShiftBitWidth) {
    if (BaseBitWidth > ShiftBitWidth)
      Shift =
        new ZExtInst(Shift, Base->getType(), "", BB);
    else
      Shift =
        new TruncInst(Shift, Base->getType(), "", BB);
  }

  auto Inst = BinaryOperator::Create(BO, Base, Shift, BV->getName(), BB);

  // For floating-point operations, if "FastMath" is enabled, set the "FastMath"
  // flags on the handled instruction
  if (SPIRVGenFastMath && isa<FPMathOperator>(Inst)) {
    llvm::FastMathFlags FMF;
    FMF.setNoNaNs();
    FMF.setAllowReciprocal();
    // Enable contraction when "NoContraction" decoration is not specified
    bool AllowContract = !BV->hasDecorate(DecorationNoContraction);
    FMF.setAllowContract(AllowContract);
    // AllowRessociation should be same with AllowContract
    FMF.setAllowReassoc(AllowContract);
    Inst->setFastMathFlags(FMF);
  }
  return Inst;
}

Instruction *SPIRVToLLVM::transCmpInst(SPIRVValue *BV, BasicBlock *BB,
                                       Function *F) {
  SPIRVCompare *BC = static_cast<SPIRVCompare *>(BV);
  assert(BB && "Invalid BB");
  SPIRVType *BT = BC->getOperand(0)->getType();
  Instruction *Inst = nullptr;
  auto OP = BC->getOpCode();
  if (isLogicalOpCode(OP))
    OP = IntBoolOpMap::rmap(OP);
  if (BT->isTypeVectorOrScalarInt() || BT->isTypeVectorOrScalarBool() ||
      BT->isTypePointer())
    Inst = new ICmpInst(*BB, CmpMap::rmap(OP),
                        transValue(BC->getOperand(0), F, BB),
                        transValue(BC->getOperand(1), F, BB));
  else if (BT->isTypeVectorOrScalarFloat())
    Inst = new FCmpInst(*BB, CmpMap::rmap(OP),
                        transValue(BC->getOperand(0), F, BB),
                        transValue(BC->getOperand(1), F, BB));
  assert(Inst && "not implemented");
  return Inst;
}

bool SPIRVToLLVM::postProcessOCL() {
  std::string DemangledName;
  SPIRVWord SrcLangVer = 0;
  BM->getSourceLanguage(&SrcLangVer);
  bool IsCpp = SrcLangVer == kOCLVer::CL21;
  for (auto I = M->begin(), E = M->end(); I != E;) {
    auto F = I++;
    if (F->hasName() && F->isDeclaration()) {
      LLVM_DEBUG(dbgs() << "[postProcessOCL sret] " << *F << '\n');
      if (F->getReturnType()->isStructTy() &&
          oclIsBuiltin(F->getName(), &DemangledName, IsCpp)) {
        if (!postProcessOCLBuiltinReturnStruct(&(*F)))
          return false;
      }
    }
  }
  for (auto I = M->begin(), E = M->end(); I != E;) {
    auto F = I++;
    if (F->hasName() && F->isDeclaration()) {
      LLVM_DEBUG(dbgs() << "[postProcessOCL func ptr] " << *F << '\n');
      auto AI = F->arg_begin();
      if (hasFunctionPointerArg(&*F, AI) && isDecoratedSPIRVFunc(&*F))
        if (!postProcessOCLBuiltinWithFuncPointer(&(*F), AI))
          return false;
    }
  }
  for (auto I = M->begin(), E = M->end(); I != E;) {
    auto F = I++;
    if (F->hasName() && F->isDeclaration()) {
      LLVM_DEBUG(dbgs() << "[postProcessOCL array arg] " << *F << '\n');
      if (hasArrayArg(&(*F)) &&
          oclIsBuiltin(F->getName(), &DemangledName, IsCpp))
        if (!postProcessOCLBuiltinWithArrayArguments(&(*F), DemangledName))
          return false;
    }
  }
  return true;
}

bool SPIRVToLLVM::postProcessOCLBuiltinReturnStruct(Function *F) {
  std::string Name = F->getName();
  F->setName(Name + ".old");
  for (auto I = F->user_begin(), E = F->user_end(); I != E;) {
    if (auto CI = dyn_cast<CallInst>(*I++)) {
      auto ST = dyn_cast<StoreInst>(*(CI->user_begin()));
      assert(ST);
      std::vector<Type *> ArgTys;
      getFunctionTypeParameterTypes(F->getFunctionType(), ArgTys);
      ArgTys.insert(ArgTys.begin(),
                    PointerType::get(F->getReturnType(), SPIRAS_Private));
      auto NewF =
          getOrCreateFunction(M, Type::getVoidTy(*Context), ArgTys, Name);
      NewF->setCallingConv(F->getCallingConv());
      auto Args = getArguments(CI);
      Args.insert(Args.begin(), ST->getPointerOperand());
      auto NewCI = CallInst::Create(NewF, Args, CI->getName(), CI);
      NewCI->setCallingConv(CI->getCallingConv());
      ST->eraseFromParent();
      CI->eraseFromParent();
    }
  }
  F->eraseFromParent();
  return true;
}

bool SPIRVToLLVM::postProcessOCLBuiltinWithFuncPointer(Function* F,
    Function::arg_iterator I) {
  auto Name = undecorateSPIRVFunction(F->getName());
  std::set<Value *> InvokeFuncPtrs;
  mutateFunctionOCL (F, [=, &InvokeFuncPtrs](
      CallInst *CI, std::vector<Value *> &Args) {
    auto ALoc = std::find_if(Args.begin(), Args.end(), [](Value * elem) {
        return isFunctionPointerType(elem->getType());
      });
    assert(ALoc != Args.end() && "Buit-in must accept a pointer to function");
    assert(isa<Function>(*ALoc) && "Invalid function pointer usage");
    Value *Ctx = ALoc[1];
    Value *CtxLen = ALoc[2];
    Value *CtxAlign = ALoc[3];
    if (Name == kOCLBuiltinName::EnqueueKernel)
      assert(Args.end() - ALoc > 3);
    else
      assert(Args.end() - ALoc > 0);
    // Erase arguments what are hanled by "spir_block_bind" according to SPIR 2.0
    Args.erase(ALoc + 1, ALoc + 4);

    InvokeFuncPtrs.insert(*ALoc);
    // There will be as many calls to spir_block_bind as how much device execution
    // bult-ins using this block. This doesn't contradict SPIR 2.0 specification.
    *ALoc = addBlockBind(M, cast<Function>(removeCast(*ALoc)),
        Ctx, CtxLen, CtxAlign, CI);
    return Name;
  });
  for (auto &I:InvokeFuncPtrs)
    eraseIfNoUse(I);
  return true;
}

bool SPIRVToLLVM::postProcessOCLBuiltinWithArrayArguments(
    Function *F, const std::string &DemangledName) {
  LLVM_DEBUG(dbgs() << "[postProcessOCLBuiltinWithArrayArguments] " << *F
                    << '\n');
  auto Attrs = F->getAttributes();
  auto Name = F->getName();
  mutateFunction(
      F,
      [=](CallInst *CI, std::vector<Value *> &Args) {
        auto FBegin =
            CI->getParent()->getParent()->begin()->getFirstInsertionPt();
        for (auto &I : Args) {
          auto T = I->getType();
          if (!T->isArrayTy())
            continue;
          auto Alloca = new AllocaInst(T, 0, "", &(*FBegin));
          new StoreInst(I, Alloca, false, CI);
          auto Zero =
              ConstantInt::getNullValue(Type::getInt32Ty(T->getContext()));
          Value *Index[] = {Zero, Zero};
          I = GetElementPtrInst::CreateInBounds(Alloca, Index, "", CI);
        }
        return Name;
      },
      nullptr, &Attrs);
  return true;
}

// ToDo: Handle unsigned integer return type. May need spec change.
Instruction *SPIRVToLLVM::postProcessOCLReadImage(SPIRVInstruction *BI,
                                                  CallInst *CI,
                                                  const std::string &FuncName) {
  AttributeList Attrs = CI->getCalledFunction()->getAttributes();
  StringRef ImageTypeName;
  bool IsDepthImage = false;
  if (isOCLImageType(
          (cast<CallInst>(CI->getOperand(0)))->getArgOperand(0)->getType(),
          &ImageTypeName))
    IsDepthImage = ImageTypeName.endswith("depth_t");
  return mutateCallInstOCL(
      M, CI,
      [=](CallInst *, std::vector<Value *> &Args, llvm::Type *&RetTy) {
        CallInst *CallSampledImg = cast<CallInst>(Args[0]);
        auto Img = CallSampledImg->getArgOperand(0);
        assert(isOCLImageType(Img->getType()));
        auto Sampler = CallSampledImg->getArgOperand(1);
        Args[0] = Img;
        Args.insert(Args.begin() + 1, Sampler);
        if (Args.size() > 4) {
          ConstantInt *ImOp = dyn_cast<ConstantInt>(Args[3]);
          ConstantFP *LodVal = dyn_cast<ConstantFP>(Args[4]);
          // Drop "Image Operands" argument.
          Args.erase(Args.begin() + 3, Args.begin() + 4);
          // If the image operand is LOD and its value is zero, drop it too.
          if (ImOp && LodVal && LodVal->isNullValue() &&
              ImOp->getZExtValue() == ImageOperandsMask::ImageOperandsLodMask)
            Args.erase(Args.begin() + 3, Args.end());
        }
        if (CallSampledImg->hasOneUse()) {
          CallSampledImg->replaceAllUsesWith(
              UndefValue::get(CallSampledImg->getType()));
          CallSampledImg->dropAllReferences();
          CallSampledImg->eraseFromParent();
        }
        Type *T = CI->getType();
        if (auto VT = dyn_cast<VectorType>(T))
          T = VT->getElementType();
        RetTy = IsDepthImage ? T : CI->getType();
        return std::string(kOCLBuiltinName::SampledReadImage) +
               (T->isFloatingPointTy() ? 'f' : 'i');
      },
      [=](CallInst *NewCI) -> Instruction * {
        if (IsDepthImage)
          return InsertElementInst::Create(
              UndefValue::get(VectorType::get(NewCI->getType(), 4)), NewCI,
              getSizet(M, 0), "", NewCI->getParent());
        return NewCI;
      },
      &Attrs);
}

CallInst *
SPIRVToLLVM::postProcessOCLWriteImage(SPIRVInstruction *BI, CallInst *CI,
                                      const std::string &DemangledName) {
  AttributeList Attrs = CI->getCalledFunction()->getAttributes();
  return mutateCallInstOCL(
      M, CI,
      [=](CallInst *, std::vector<Value *> &Args) {
        llvm::Type *T = Args[2]->getType();
        if (Args.size() > 4) {
          ConstantInt *ImOp = dyn_cast<ConstantInt>(Args[3]);
          ConstantFP *LodVal = dyn_cast<ConstantFP>(Args[4]);
          // Drop "Image Operands" argument.
          Args.erase(Args.begin() + 3, Args.begin() + 4);
          // If the image operand is LOD and its value is zero, drop it too.
          if (ImOp && LodVal && LodVal->isNullValue() &&
              ImOp->getZExtValue() == ImageOperandsMask::ImageOperandsLodMask)
            Args.erase(Args.begin() + 3, Args.end());
          else
            std::swap(Args[2], Args[3]);
        }
        return std::string(kOCLBuiltinName::WriteImage) +
               (T->isFPOrFPVectorTy() ? 'f' : 'i');
      },
      &Attrs);
}

CallInst *SPIRVToLLVM::postProcessOCLBuildNDRange(SPIRVInstruction *BI,
                                                  CallInst *CI,
                                                  const std::string &FuncName) {
  assert(CI->getNumArgOperands() == 3);
  auto GWS = CI->getArgOperand(0);
  auto LWS = CI->getArgOperand(1);
  auto GWO = CI->getArgOperand(2);
  CI->setArgOperand(0, GWO);
  CI->setArgOperand(1, GWS);
  CI->setArgOperand(2, LWS);
  return CI;
}

Instruction *
SPIRVToLLVM::postProcessGroupAllAny(CallInst *CI,
                                    const std::string &DemangledName) {
  AttributeList Attrs = CI->getCalledFunction()->getAttributes();
  return mutateCallInstSPIRV(
      M, CI,
      [=](CallInst *, std::vector<Value *> &Args, llvm::Type *&RetTy) {
        Type *Int32Ty = Type::getInt32Ty(*Context);
        RetTy = Int32Ty;
        Args[1] = CastInst::CreateZExtOrBitCast(Args[1], Int32Ty, "", CI);
        return DemangledName;
      },
      [=](CallInst *NewCI) -> Instruction * {
        Type *RetTy = Type::getInt1Ty(*Context);
        return CastInst::CreateTruncOrBitCast(NewCI, RetTy, "",
                                              NewCI->getNextNode());
      },
      &Attrs);
}

CallInst *
SPIRVToLLVM::expandOCLBuiltinWithScalarArg(CallInst *CI,
                                           const std::string &FuncName) {
  AttributeList Attrs = CI->getCalledFunction()->getAttributes();
  if (!CI->getOperand(0)->getType()->isVectorTy() &&
      CI->getOperand(1)->getType()->isVectorTy()) {
    return mutateCallInstOCL(
        M, CI,
        [=](CallInst *, std::vector<Value *> &Args) {
          unsigned VecSize =
              CI->getOperand(1)->getType()->getVectorNumElements();
          Value *NewVec = nullptr;
          if (auto CA = dyn_cast<Constant>(Args[0]))
            NewVec = ConstantVector::getSplat(VecSize, CA);
          else {
            NewVec = ConstantVector::getSplat(
                VecSize, Constant::getNullValue(Args[0]->getType()));
            NewVec = InsertElementInst::Create(NewVec, Args[0], getInt32(M, 0),
                                               "", CI);
            NewVec = new ShuffleVectorInst(
                NewVec, NewVec,
                ConstantVector::getSplat(VecSize, getInt32(M, 0)), "", CI);
          }
          NewVec->takeName(Args[0]);
          Args[0] = NewVec;
          return FuncName;
        },
        &Attrs);
  }
  return CI;
}

std::string
SPIRVToLLVM::transOCLPipeTypeAccessQualifier(SPIRV::SPIRVTypePipe *ST) {
  return SPIRSPIRVAccessQualifierMap::rmap(ST->getAccessQualifier());
}

void SPIRVToLLVM::transGeneratorMD() {
  SPIRVMDBuilder B(*M);
  B.addNamedMD(kSPIRVMD::Generator)
      .addOp()
      .addU16(BM->getGeneratorId())
      .addU16(BM->getGeneratorVer())
      .done();
}

Value *SPIRVToLLVM::oclTransConstantSampler(SPIRV::SPIRVConstantSampler *BCS) {
  auto Lit = (BCS->getAddrMode() << 1) | BCS->getNormalized() |
             ((BCS->getFilterMode() + 1) << 4);
  auto Ty = IntegerType::getInt32Ty(*Context);
  return ConstantInt::get(Ty, Lit);
}

Value *SPIRVToLLVM::oclTransConstantPipeStorage(
    SPIRV::SPIRVConstantPipeStorage *BCPS) {

  string CPSName = string(kSPIRVTypeName::PrefixAndDelim) +
                   kSPIRVTypeName::ConstantPipeStorage;

  auto Int32Ty = IntegerType::getInt32Ty(*Context);
  auto CPSTy = M->getTypeByName(CPSName);
  if (!CPSTy) {
    Type *CPSElemsTy[] = {Int32Ty, Int32Ty, Int32Ty};
    CPSTy = StructType::create(*Context, CPSElemsTy, CPSName);
  }

  assert(CPSTy != nullptr && "Could not create spirv.ConstantPipeStorage");

  Constant *CPSElems[] = {ConstantInt::get(Int32Ty, BCPS->getPacketSize()),
                          ConstantInt::get(Int32Ty, BCPS->getPacketAlign()),
                          ConstantInt::get(Int32Ty, BCPS->getCapacity())};

  return new GlobalVariable(*M, CPSTy, false, GlobalValue::LinkOnceODRLinkage,
                            ConstantStruct::get(CPSTy, CPSElems),
                            BCPS->getName(), nullptr,
                            GlobalValue::NotThreadLocal, SPIRAS_Global);
}

/// For instructions, this function assumes they are created in order
/// and appended to the given basic block. An instruction may use a
/// instruction from another BB which has not been translated. Such
/// instructions should be translated to place holders at the point
/// of first use, then replaced by real instructions when they are
/// created.
///
/// When CreatePlaceHolder is true, create a load instruction of a
/// global variable as placeholder for SPIRV instruction. Otherwise,
/// create instruction and replace placeholder if there is one.
Value *SPIRVToLLVM::transValueWithoutDecoration(SPIRVValue *BV, Function *F,
                                                BasicBlock *BB,
                                                bool CreatePlaceHolder) {

  auto OC = BV->getOpCode();
  IntBoolOpMap::rfind(OC, &OC);

  // Translation of non-instruction values
  switch(OC) {
  case OpConstant:
  case OpSpecConstant: {
    SPIRVConstant *BConst = static_cast<SPIRVConstant *>(BV);
    SPIRVType *BT = BV->getType();
    Type *LT = transType(BT);
    switch (BT->getOpCode()) {
    case OpTypeBool:
    case OpTypeInt:
      return mapValue(
          BV, ConstantInt::get(LT, BConst->getZExtIntValue(),
                               static_cast<SPIRVTypeInt *>(BT)->isSigned()));
    case OpTypeFloat: {
      const llvm::fltSemantics *FS = nullptr;
      switch (BT->getFloatBitWidth()) {
      case 16:
        FS = &APFloat::IEEEhalf();
        break;
      case 32:
        FS = &APFloat::IEEEsingle();
        break;
      case 64:
        FS = &APFloat::IEEEdouble();
        break;
      default:
        llvm_unreachable("invalid float type");
      }
      return mapValue(
          BV, ConstantFP::get(*Context,
                              APFloat(*FS, APInt(BT->getFloatBitWidth(),
                                                 BConst->getZExtIntValue()))));
    }
    default:
      llvm_unreachable("Not implemented");
      return nullptr;
    }
  }

  case OpConstantTrue:
  case OpConstantFalse:
  case OpSpecConstantTrue:
  case OpSpecConstantFalse: {
    bool BoolVal = (OC == OpConstantTrue || OC == OpSpecConstantTrue) ?
                      static_cast<SPIRVConstantTrue *>(BV)->getBoolValue() :
                      static_cast<SPIRVConstantFalse *>(BV)->getBoolValue();
    return BoolVal ? mapValue(BV, ConstantInt::getTrue(*Context)) :
                     mapValue(BV, ConstantInt::getFalse(*Context));
  }

  case OpConstantNull: {
    auto LT = transType(BV->getType());
    return mapValue(BV, Constant::getNullValue(LT));
  }

  case OpConstantComposite:
  case OpSpecConstantComposite: {
    auto BCC = static_cast<SPIRVConstantComposite *>(BV);
    std::vector<Constant *> CV;
    for (auto &I : BCC->getElements())
      CV.push_back(dyn_cast<Constant>(transValue(I, F, BB)));
    switch (BV->getType()->getOpCode()) {
    case OpTypeVector:
      return mapValue(BV, ConstantVector::get(CV));
    case OpTypeArray:
      for (auto &C : CV)
        C = widenBoolConstant(C);
      return mapValue(BV, ConstantArray::get(
          dyn_cast<ArrayType>(transType(BCC->getType())), CV));
    case OpTypeStruct: {
      for (auto &C : CV)
        C = widenBoolConstant(C);
      auto BCCTy = dyn_cast<StructType>(transType(BCC->getType()));
      auto Members = BCCTy->getNumElements();
      auto Constants = CV.size();
      //if we try to initialize constant TypeStruct, add bitcasts
      //if src and dst types are both pointers but to different types
      if (Members == Constants) {
        for (unsigned I = 0; I < Members; ++I) {
          if (CV[I]->getType() == BCCTy->getElementType(I))
            continue;
          if (!CV[I]->getType()->isPointerTy() ||
              !BCCTy->getElementType(I)->isPointerTy())
            continue;

          CV[I] = ConstantExpr::getBitCast(CV[I], BCCTy->getElementType(I));
        }
      }

      return mapValue(BV,
                      ConstantStruct::get(
                          dyn_cast<StructType>(transType(BCC->getType())), CV));
    }
    case OpTypeMatrix: {
      return mapValue(BV, ConstantArray::get(
        dyn_cast<ArrayType>(transType(BCC->getType())), CV));
    }
    default:
      llvm_unreachable("not implemented");
      return nullptr;
    }
  }

  case OpConstantSampler: {
    auto BCS = static_cast<SPIRVConstantSampler *>(BV);
    return mapValue(BV, oclTransConstantSampler(BCS));
  }

  case OpConstantPipeStorage: {
    auto BCPS = static_cast<SPIRVConstantPipeStorage *>(BV);
    return mapValue(BV, oclTransConstantPipeStorage(BCPS));
  }

  case OpSpecConstantOp: {
    if (!IsKernel) {
      auto BI = static_cast<SPIRVSpecConstantOp*>(BV)->getMappedConstant();
      return mapValue(BV, transValue(BI, nullptr, nullptr, false));
    } else {
      auto BI = createInstFromSpecConstantOp(
          static_cast<SPIRVSpecConstantOp*>(BV));
      return mapValue(BV, transValue(BI, nullptr, nullptr, false));
    }
  }

  case OpUndef:
    return mapValue(BV, UndefValue::get(transType(BV->getType())));

  case OpVariable: {
    auto BVar = static_cast<SPIRVVariable *>(BV);
    auto Ty = widenBoolType(transType(BVar->getType()->getPointerElementType()));
    bool IsConst = BVar->isConstant();
    llvm::GlobalValue::LinkageTypes LinkageTy = transLinkageType(BVar);
    Constant *Initializer = nullptr;
    SPIRVValue *Init = BVar->getInitializer();
    if (Init) {
      Initializer = dyn_cast<Constant>(transValue(Init, F, BB, false));
      Initializer = widenBoolConstant(Initializer);
    }
    else if (LinkageTy == GlobalValue::CommonLinkage)
      // In LLVM variables with common linkage type must be initilized by 0
      Initializer = Constant::getNullValue(Ty);
    else if (BVar->getStorageClass() == SPIRVStorageClassKind::StorageClassWorkgroup ||
             LinkageTy == GlobalValue::InternalLinkage ||
             LinkageTy == GlobalValue::PrivateLinkage)
      Initializer = dyn_cast<Constant>(UndefValue::get(Ty));

    SPIRVStorageClassKind BS = BVar->getStorageClass();
    if (BS == StorageClassFunction) {
      assert (BB && "Invalid BB");
      auto LVar = mapValue(BV, new AllocaInst(
        Ty, M->getDataLayout().getAllocaAddrSpace(),
        BV->getName(), BB));
      if (Init)
        new StoreInst(Initializer, LVar, BB);
      return LVar;
    }
    auto AddrSpace = SPIRSPIRVAddrSpaceMap::rmap(BS);
    auto LVar = new GlobalVariable(*M, Ty, IsConst, LinkageTy, Initializer,
                                   BV->getName(), 0,
                                   GlobalVariable::NotThreadLocal, AddrSpace);
    LVar->setUnnamedAddr((IsConst && Ty->isArrayTy() &&
                          Ty->getArrayElementType()->isIntegerTy(8))
                             ? GlobalValue::UnnamedAddr::Global
                             : GlobalValue::UnnamedAddr::None);

    SPIRVBuiltinVariableKind BVKind;
    if (BVar->isBuiltin(&BVKind))
      BuiltinGVMap[LVar] = BVKind;
    return mapValue(BV, LVar);
  }

  case OpFunctionParameter: {
    auto BA = static_cast<SPIRVFunctionParameter *>(BV);
    assert(F && "Invalid function");
    unsigned ArgNo = 0;
    for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
         ++I, ++ArgNo) {
      if (ArgNo == BA->getArgNo())
        return mapValue(BV, &(*I));
    }
    llvm_unreachable("Invalid argument");
    return nullptr;
  }

  case OpFunction:
    return mapValue(BV, transFunction(static_cast<SPIRVFunction *>(BV)));

  case OpLabel:
    return mapValue(BV, BasicBlock::Create(*Context, BV->getName(), F));

  default:
    // do nothing
    break;
  }

  // During translation of OpSpecConstantOp we create an instruction
  // corresponding to the Opcode operand and then translate this instruction.
  // For such instruction BB and F should be nullptr, because it is a constant
  // expression declared out of scope of any basic block or function.
  // All other values require valid BB pointer.
  assert(((isSpecConstantOpAllowedOp(OC) && !F && !BB) || BB) && "Invalid BB");

  // Creation of place holder
  if (CreatePlaceHolder) {
    auto GV = new GlobalVariable(
        *M, transType(BV->getType()), false, GlobalValue::PrivateLinkage,
        nullptr, std::string(KPlaceholderPrefix) + BV->getName(), 0,
        GlobalVariable::NotThreadLocal, 0);
    auto LD = new LoadInst(GV, BV->getName(), BB);
    PlaceholderMap[BV] = LD;
    return mapValue(BV, LD);
  }

  // Translation of instructions
  switch (static_cast<uint32_t>(BV->getOpCode())) {
  case OpBranch: {
    auto BR = static_cast<SPIRVBranch *>(BV);
    auto BI = BranchInst::Create(
        dyn_cast<BasicBlock>(transValue(BR->getTargetLabel(), F, BB)), BB);
    auto LM = static_cast<SPIRVLoopMerge *>(BR->getPrevious());
    if (LM != nullptr && LM->getOpCode() == OpLoopMerge)
      setLLVMLoopMetadata(LM, BI);
    else if (BR->getBasicBlock()->getLoopMerge())
      setLLVMLoopMetadata(BR->getBasicBlock()->getLoopMerge(), BI);
    return mapValue(BV, BI);
  }

  case OpBranchConditional: {
    auto BR = static_cast<SPIRVBranchConditional *>(BV);
    auto C = transValue(BR->getCondition(), F, BB);

    // Workaround a bug where old shader compilers would sometimes specify
    // int/float arguments as the branch condition
    if (SPIRVWorkaroundBadSPIRV) {
      if (C->getType()->isFloatTy())
        C = new llvm::FCmpInst(*BB, llvm::CmpInst::FCMP_ONE, C,
          llvm::ConstantFP::get(C->getType(), 0.0));
      else if (C->getType()->isIntegerTy() && !C->getType()->isIntegerTy(1))
        C = new llvm::ICmpInst(*BB, llvm::CmpInst::ICMP_NE, C,
          llvm::ConstantInt::get(C->getType(), 0));
    }

    auto BC = BranchInst::Create(
        dyn_cast<BasicBlock>(transValue(BR->getTrueLabel(), F, BB)),
        dyn_cast<BasicBlock>(transValue(BR->getFalseLabel(), F, BB)),
        C, BB);
    auto LM = static_cast<SPIRVLoopMerge *>(BR->getPrevious());
    if (LM != nullptr && LM->getOpCode() == OpLoopMerge)
      setLLVMLoopMetadata(LM, BC);
    else if (BR->getBasicBlock()->getLoopMerge())
      setLLVMLoopMetadata(BR->getBasicBlock()->getLoopMerge(), BC);
    return mapValue(BV, BC);
  }

  case OpPhi: {
    auto Phi = static_cast<SPIRVPhi *>(BV);
    PHINode* PhiNode = nullptr;
    if (BB->getFirstInsertionPt() != BB->end())
      PhiNode = PHINode::Create(transType(Phi->getType()),
                                Phi->getPairs().size() / 2,
                                Phi->getName(),
                                &*BB->getFirstInsertionPt());
    else
      PhiNode = PHINode::Create(transType(Phi->getType()),
                                Phi->getPairs().size() / 2, Phi->getName(), BB);

    auto LPhi = dyn_cast<PHINode>(mapValue(BV, PhiNode));
    Phi->foreachPair([&](SPIRVValue *IncomingV, SPIRVBasicBlock *IncomingBB,
                         size_t Index) {
      auto Translated = transValue(IncomingV, F, BB);
      LPhi->addIncoming(Translated,
                        dyn_cast<BasicBlock>(transValue(IncomingBB, F, BB)));
    });
    return LPhi;
  }

  case OpUnreachable:
    return mapValue(BV, new UnreachableInst(*Context, BB));

  case OpReturn:
    return mapValue(BV, ReturnInst::Create(*Context, BB));

  case OpReturnValue: {
    auto RV = static_cast<SPIRVReturnValue *>(BV);
    return mapValue(
        BV, ReturnInst::Create(*Context,
                               transValue(RV->getReturnValue(), F, BB), BB));
  }
  case OpKill: {
    auto Kill = mapValue(BV, transSPIRVBuiltinFromInst(
      static_cast<SPIRVInstruction *>(BV), BB));

    // NOTE: In SPIR-V, "OpKill" is considered as a valid instruction to
    // terminate blocks. But in LLVM, we have to insert a dummy "return"
    // instruction as block terminator.
    if (F->getReturnType()->isVoidTy())
      // No return
      ReturnInst::Create(*Context, BB);
    else
      // Function returns value
      ReturnInst::Create(*Context, UndefValue::get(F->getReturnType()), BB);
    return Kill;
  }
  case OpLifetimeStart: {
    SPIRVLifetimeStart *LTStart = static_cast<SPIRVLifetimeStart *>(BV);
    IRBuilder<> Builder(BB);
    SPIRVWord Size = LTStart->getSize();
    ConstantInt *S = nullptr;
    if (Size)
      S = Builder.getInt64(Size);
    Value *Var = transValue(LTStart->getObject(), F, BB);
    CallInst *Start = Builder.CreateLifetimeStart(Var, S);
    return mapValue(BV, Start->getOperand(1));
  }

  case OpLifetimeStop: {
    SPIRVLifetimeStop *LTStop = static_cast<SPIRVLifetimeStop *>(BV);
    IRBuilder<> Builder(BB);
    SPIRVWord Size = LTStop->getSize();
    ConstantInt *S = nullptr;
    if (Size)
      S = Builder.getInt64(Size);
    auto Var = transValue(LTStop->getObject(), F, BB);
    for (const auto &I : Var->users())
      if (auto II = getLifetimeStartIntrinsic(dyn_cast<Instruction>(I)))
        return mapValue(BV, Builder.CreateLifetimeEnd(II->getOperand(1), S));
    return mapValue(BV, Builder.CreateLifetimeEnd(Var, S));
  }

  case OpStore: {
    SPIRVStore *BS = static_cast<SPIRVStore*>(BV);
    Instruction *SI = nullptr;
    auto Src = transValue(BS->getSrc(), F, BB);
    Src = widenBoolValue(Src, BB);
    auto Dst = transValue(BS->getDst(), F, BB);

    // NOTE: For those storage classes that will not involve memory
    // operations, we clear "volatile" access mask.
    bool IsVolatile = BS->SPIRVMemoryAccess::isVolatile();
    SPIRVStorageClassKind StorageClass =
      BS->getDst()->getType()->getPointerStorageClass();
    if (StorageClass == StorageClassInput ||
        StorageClass == StorageClassOutput ||
        StorageClass == StorageClassPrivate ||
        StorageClass == StorageClassFunction)
      IsVolatile = false;

    // NOTE: If the store destination is a structure generated from AccessChain,
    // use special buffer store call to store the result of AccessChain.
    // Otherwise, use normal store instruction.
    if ((StorageClass == StorageClassStorageBuffer) &&
      Dst->getType()->isStructTy()) {
      auto Inst = dyn_cast<Instruction>(Dst);
      auto MD = Inst->getMetadata(gSPIRVMD::Block);
      std::string  MangledFuncName = "";
      mangleGlslBuiltin(gSPIRVMD::BufferStore, { Src->getType() }, MangledFuncName);
      auto NewF = getOrCreateFunction(M, Type::getVoidTy(*Context),
        { Src->getType(), Dst->getType() }, MangledFuncName);
      NewF->setCallingConv(CallingConv::SPIR_FUNC);
      auto V = CallInst::Create(NewF, { Src, Dst }, "", BB);
      V->setMetadata(gSPIRVMD::Block, MD);
      return mapValue(BV, V);
    }
    else {
      SI = new StoreInst(Src, Dst, IsVolatile,
        BS->SPIRVMemoryAccess::getAlignment(), BB);

      if (BS->SPIRVMemoryAccess::isNonTemporal())
        transNonTemporalMetadata(SI);
      return mapValue(BV, SI);
    }
  }

  case OpLoad: {
    SPIRVLoad *BL = static_cast<SPIRVLoad*>(BV);

    // NOTE: For those storage classes that will not involve memory
    // operations, we clear "volatile" access mask.
    bool IsVolatile = BL->SPIRVMemoryAccess::isVolatile();
    SPIRVStorageClassKind StorageClass =
      BL->getSrc()->getType()->getPointerStorageClass();
    if (StorageClass == StorageClassInput ||
        StorageClass == StorageClassOutput ||
        StorageClass == StorageClassPrivate ||
        StorageClass == StorageClassFunction)
      IsVolatile = false;
    auto LoadSrc = transValue(BL->getSrc(), F, BB);

    // NOTE: If the load source is a structure generated from AccessChain,
    // use special buffer load call to load the result of AccessChain,
    // Otherwise, use normal load instruction.
    if ((StorageClass == StorageClassStorageBuffer) &&
      LoadSrc->getType()->isStructTy()){
      auto Inst = dyn_cast<Instruction>(LoadSrc);
      auto MD = Inst->getMetadata(gSPIRVMD::Block);
      auto ElemTy = transType(BL->getSrc()->getType()->getPointerElementType());
      auto MangledTy = { ElemTy };
      std::string MangledFuncName = "";
      mangleGlslBuiltin(gSPIRVMD::BufferLoad, MangledTy, MangledFuncName);
      auto NewF = getOrCreateFunction(M, ElemTy,
        { LoadSrc->getType()}, MangledFuncName);

      NewF->setCallingConv(CallingConv::SPIR_FUNC);
      auto V = CallInst::Create(NewF, { LoadSrc }, "", BB);
      V->setMetadata(gSPIRVMD::Block, MD);
      return mapValue(BV, V);
    } else {
      LoadInst *LI = new LoadInst(LoadSrc, BV->getName(),
        IsVolatile,
        BL->SPIRVMemoryAccess::getAlignment(), BB);
      if (BL->SPIRVMemoryAccess::isNonTemporal())
        transNonTemporalMetadata(LI);
      return mapValue(BV,
        narrowBoolValue(LI, BL->getSrc()->getType()->getPointerElementType(), BB));
    }
  }

  case OpCopyMemory: {
    SPIRVCopyMemory *CM = static_cast<SPIRVCopyMemory *>(BV);
    LoadInst* LI = new LoadInst(transValue(CM->getSource(), F, BB), "", BB);
    StoreInst* SI = new StoreInst(LI, transValue(CM->getTarget(), F, BB),
                                  false, BB);
    return mapValue(BV, SI);
  }

  case OpCopyMemorySized: {
    SPIRVCopyMemorySized *BC = static_cast<SPIRVCopyMemorySized *>(BV);
    CallInst *CI = nullptr;
    llvm::Value *Dst = transValue(BC->getTarget(), F, BB);
    unsigned Align = BC->getAlignment();
    llvm::Value *Size = transValue(BC->getSize(), F, BB);
    bool IsVolatile = BC->SPIRVMemoryAccess::isVolatile();
    IRBuilder<> Builder(BB);

    // If we copy from zero-initialized array, we can optimize it to llvm.memset
    if (BC->getSource()->getOpCode() == OpBitcast) {
      SPIRVValue *Source =
          static_cast<SPIRVBitcast *>(BC->getSource())->getOperand(0);
      if (Source->isVariable()) {
        auto *Init = static_cast<SPIRVVariable *>(Source)->getInitializer();
        if (isa<OpConstantNull>(Init)) {
          SPIRVType *Ty = static_cast<SPIRVConstantNull *>(Init)->getType();
          if (isa<OpTypeArray>(Ty)) {
            SPIRVTypeArray *AT = static_cast<SPIRVTypeArray *>(Ty);
            Type *SrcTy = transType(AT->getArrayElementType());
            assert(SrcTy->isIntegerTy(8));
            llvm::Value *Src = ConstantInt::get(SrcTy, 0);
            CI = Builder.CreateMemSet(Dst, Src, Size, Align, IsVolatile);
          }
        }
      }
    }
    if (!CI) {
      llvm::Value *Src = transValue(BC->getSource(), F, BB);
      CI = Builder.CreateMemCpy(Dst, Align, Src, Align, Size, IsVolatile);
    }
    if (isFuncNoUnwind())
      CI->getFunction()->addFnAttr(Attribute::NoUnwind);
    return mapValue(BV, CI);
  }

  case OpSelect: {
    SPIRVSelect *BS = static_cast<SPIRVSelect *>(BV);
    return mapValue(BV,
                    SelectInst::Create(transValue(BS->getCondition(), F, BB),
                                       transValue(BS->getTrueValue(), F, BB),
                                       transValue(BS->getFalseValue(), F, BB),
                                       BV->getName(), BB));
  }

  case OpLine:
  case OpSelectionMerge: // OpenCL Compiler does not use this instruction
    return nullptr;
  case OpLoopMerge: {      // Should be translated at OpBranch or OpBranchConditional cases
    SPIRVLoopMerge *LM = static_cast<SPIRVLoopMerge *>(BV);
    auto Label = BM->get<SPIRVBasicBlock>(LM->getContinueTarget());
    Label->setLoopMerge(LM);
    return nullptr;
  }
  case OpSwitch: {
    auto BS = static_cast<SPIRVSwitch *>(BV);
    auto Select = transValue(BS->getSelect(), F, BB);
    auto LS = SwitchInst::Create(
        Select, dyn_cast<BasicBlock>(transValue(BS->getDefault(), F, BB)),
        BS->getNumPairs(), BB);
    BS->foreachPair(
        [&](SPIRVSwitch::LiteralTy Literals, SPIRVBasicBlock *Label) {
          assert(!Literals.empty() && "Literals should not be empty");
          assert(Literals.size() <= 2 &&
                 "Number of literals should not be more then two");
          uint64_t Literal = uint64_t(Literals.at(0));
          if (Literals.size() == 2) {
            Literal += uint64_t(Literals.at(1)) << 32;
          }
          LS->addCase(ConstantInt::get(dyn_cast<IntegerType>(Select->getType()),
                                       Literal),
                      dyn_cast<BasicBlock>(transValue(Label, F, BB)));
        });
    return mapValue(BV, LS);
  }

  case OpVectorTimesScalar: {
    auto VTS = static_cast<SPIRVVectorTimesScalar *>(BV);
    IRBuilder<> Builder(BB);
    auto Scalar = transValue(VTS->getScalar(), F, BB);
    auto Vector = transValue(VTS->getVector(), F, BB);
    assert(Vector->getType()->isVectorTy() && "Invalid type");
    unsigned VecSize = Vector->getType()->getVectorNumElements();
    auto NewVec = Builder.CreateVectorSplat(VecSize, Scalar, Scalar->getName());
    NewVec->takeName(Scalar);
    auto Scale = Builder.CreateFMul(Vector, NewVec, "scale");
    return mapValue(BV, Scale);
  }

  case OpCopyObject: {
    SPIRVCopyObject *CO = static_cast<SPIRVCopyObject *>(BV);
    AllocaInst* AI = nullptr;
    // NOTE: Alloc instructions not in the entry block will prevent LLVM from doing function
    // inlining. Try to move those alloc instructions to the entry block.
    auto FirstInst = BB->getParent()->getEntryBlock().getFirstInsertionPt();
    if (FirstInst != BB->getParent()->getEntryBlock().end())
      AI = new AllocaInst(transType(CO->getOperand()->getType()),
                          M->getDataLayout().getAllocaAddrSpace(),
                          "",
                          &*FirstInst);
    else
      AI = new AllocaInst(transType(CO->getOperand()->getType()),
                          M->getDataLayout().getAllocaAddrSpace(),
                          "",
                          BB);

    new StoreInst(transValue(CO->getOperand(), F, BB), AI, BB);
    LoadInst *LI = new LoadInst(AI, "", BB);
    return mapValue(BV, LI);
  }

  case OpAccessChain:
  case OpInBoundsAccessChain:
  case OpPtrAccessChain:
  case OpInBoundsPtrAccessChain: {
    auto AC = static_cast<SPIRVAccessChainBase *>(BV);
    auto Base = transValue(AC->getBase(), F, BB);
    auto Index = transValue(AC->getIndices(), F, BB);
    truncConstantIndex(Index, BB);
    if (!AC->hasPtrIndex())
        Index.insert(Index.begin(), getInt32(M, 0));
    auto IsInbound = AC->isInBounds();
    Value *V = nullptr;
    // Apply special processing for pointer only when capability
    // "StorageClassStorageBuffer" is declared
    bool UseVarPtr = EnableVarPtr && (AC->getBase()->getType()->getPointerStorageClass()
      == StorageClassStorageBuffer);
    if (UseVarPtr == false) {
      if (BB) {
        auto GEP =
          GetElementPtrInst::Create(nullptr, Base, Index, BV->getName(), BB);
        GEP->setIsInBounds(IsInbound);
        V = GEP;
      } else {
        V = ConstantExpr::getGetElementPtr(nullptr, dyn_cast<Constant>(Base), Index,
                                           IsInbound);
      }
    } else {
      // NOTE: For variable pointer storage buffer, create special calls instead of using GEP instruction.
      // <descriptor, offset> = @spirv.AccessChain(...)
      assert(BB != nullptr);
      auto BBTy = AC->getBase()->getType();

      assert(DescriptorSizeBuffer == 4);
      auto Vec4Ty = VectorType::get(Type::getInt32Ty(*Context), DescriptorSizeBuffer);
      SmallVector<Type *, 4> MT;
      MT.push_back(Vec4Ty);
      MT.push_back(Type::getInt32Ty(*Context));
      // Pointer to storage buffer will be converted to the structure <descriptor, offset>
      Type* StructTy = StructType::get(*Context, MT);
      // Create function to return storage buffer pointer structure {descriptor, offset}
      const uint32_t IndexArgOffst = 2; // Indices begin from 2 (the first is base Type, the second
                                        // is a pointer to the base type)
      // NOTE: For variable pointer storage buffer, the base is a pointer type,
      // so one additional index with zero value is needed.
      Index.insert(Index.begin(), getInt32(M, 0));

      std::vector<Type *> ArgTys(Index.size() + IndexArgOffst);
      ArgTys[0] = Base->getType();

      // Keep the info of both base type and the pointer to base type of GEP instrcution
      ArgTys[1] = PointerType::get(transType(AC->getBase()->getType()->getPointerElementType()),
        SPIRSPIRVAddrSpaceMap::rmap(AC->getType()->getPointerStorageClass()));

      for (auto I = 0; I < Index.size(); ++I)
          ArgTys[I + IndexArgOffst] = Index[I]->getType();

      std::string MangledName = "";
      uint32_t MangleIdx = 0;
      mangleGlslBuiltin(gSPIRVMD::AccessChain, ArgTys, MangledName);
      if (MangleNameToIndex.find(MangledName) == MangleNameToIndex.end()) {
        MangleIdx = MangleNameToIndex.size();
        MangleNameToIndex[MangledName] = MangleIdx;
      } else
          MangleIdx = MangleNameToIndex[MangledName];
      MangledName = gSPIRVMD::AccessChain + std::to_string(MangleIdx);
      Function *Func = M->getFunction(MangledName);
      if (Func == nullptr) {
          auto FuncTy = FunctionType::get(StructTy, ArgTys, false);
          Func = Function::Create(FuncTy, GlobalValue::ExternalLinkage, MangledName, M);
          Func->setCallingConv(CallingConv::SPIR_FUNC);
          Func->addFnAttr(Attribute::NoUnwind);
      }
      std::vector<Value *> Args(Index.size() + IndexArgOffst);
      Args[0] = Base;
      Args[1] = Constant::getNullValue(ArgTys[1]);
      std::copy(Index.begin(), Index.end(), Args.begin() + IndexArgOffst);
      V = CallInst::Create(Func, Args, "", BB);
    }

    return mapValue(BV, V);
  }

  case OpCompositeConstruct: {
    auto CC = static_cast<SPIRVCompositeConstruct *>(BV);
    auto Constituents = transValue(CC->getConstituents(), F, BB);
    std::vector<Constant *> CV;
    for (const auto &I : Constituents) {
      CV.push_back(dyn_cast<Constant>(I));
    }
    switch (BV->getType()->getOpCode()) {
    case OpTypeVector: {
      auto VecTy = transType(CC->getType());
      Value* V = UndefValue::get(VecTy);
      for (uint32_t Idx = 0, I = 0, E = Constituents.size(); I < E; ++I) {
        if (Constituents[I]->getType()->isVectorTy()) {
          // NOTE: It is allowed to construct a vector from several "smaller"
          // scalars or vectors, such as vec4 = (vec2, vec2) or vec4 = (float,
          // vec3).
          auto CompCount = Constituents[I]->getType()->getVectorNumElements();
          for (uint32_t J = 0; J < CompCount; ++J) {
            auto Comp = ExtractElementInst::Create(Constituents[I],
                          ConstantInt::get(*Context, APInt(32, J)),
                          "", BB);
            V = InsertElementInst::Create(V, Comp,
                  ConstantInt::get(*Context, APInt(32, Idx)), "", BB);
            ++Idx;
          }
        } else {
          V = InsertElementInst::Create(V, Constituents[I],
                ConstantInt::get(*Context, APInt(32, Idx)), "", BB);
          ++Idx;
        }
      }
      return mapValue(BV, V);
    }
    case OpTypeArray:
    case OpTypeStruct: {
      auto CCTy = transType(CC->getType());
      Value *V = UndefValue::get(CCTy);
      for (size_t I = 0, E = Constituents.size(); I < E; ++I) {
        V = InsertValueInst::Create(V, widenBoolValue(Constituents[I], BB),
            I, "", BB);
      }
      return mapValue(BV, V);
    }
    case OpTypeMatrix: {
      auto BVTy = BV->getType();
      auto MatClmTy = transType(BVTy->getMatrixColumnType());
      auto MatCount = BVTy->getMatrixColumnCount();
      auto MatTy = ArrayType::get(MatClmTy, MatCount);

      auto MatCountVal = ConstantInt::get(*Context, APInt(32, MatCount));
      Value* V = UndefValue::get(MatTy);
      for (uint32_t I = 0, E = Constituents.size(); I < E; ++I) {
          V = InsertValueInst::Create(V, widenBoolValue(Constituents[I], BB),
              I, "", BB);
      }
      return mapValue(BV, V);
      }
    default:
      llvm_unreachable("Unhandled type!");
    }
  }

  case OpCompositeExtract: {
    SPIRVCompositeExtract *CE = static_cast<SPIRVCompositeExtract *>(BV);
    if (CE->getComposite()->getType()->isTypeVector()) {
      assert(CE->getIndices().size() == 1 && "Invalid index");
      return mapValue(
          BV, ExtractElementInst::Create(
                  transValue(CE->getComposite(), F, BB),
                  ConstantInt::get(*Context, APInt(32, CE->getIndices()[0])),
                  BV->getName(), BB));
    } else {
      auto CV = transValue(CE->getComposite(), F, BB);
      auto IndexedTy = ExtractValueInst::getIndexedType(
        CV->getType(), CE->getIndices());
      if (IndexedTy == nullptr) {
        // NOTE: "OpCompositeExtract" could extract a scalar component from a
        // vector or a vector in an aggregate. But in LLVM, "extractvalue" is
        // unable to do such thing. We have to replace it with "extractelement"
        // + "extractelement" to achieve this purpose.
        assert(CE->getType()->isTypeScalar());
        std::vector<SPIRVWord> Idxs = CE->getIndices();
        auto LastIdx = Idxs.back();
        Idxs.pop_back();

        Value* V = ExtractValueInst::Create(CV, Idxs, "", BB);
        assert(V->getType()->isVectorTy());
        return mapValue(BV, narrowBoolValue(ExtractElementInst::Create(
          V, ConstantInt::get(*Context, APInt(32, LastIdx)),
          BV->getName(), BB), CE->getType(), BB));
      } else
        return mapValue(BV, narrowBoolValue(ExtractValueInst::Create(
          CV, CE->getIndices(), BV->getName(), BB), CE->getType(), BB));
    }
  }

  case OpVectorExtractDynamic: {
    auto CE = static_cast<SPIRVVectorExtractDynamic *>(BV);
    return mapValue(
        BV, ExtractElementInst::Create(transValue(CE->getVector(), F, BB),
                                       transValue(CE->getIndex(), F, BB),
                                       BV->getName(), BB));
  }

  case OpCompositeInsert: {
    auto CI = static_cast<SPIRVCompositeInsert *>(BV);
    if (CI->getComposite()->getType()->isTypeVector()) {
      assert(CI->getIndices().size() == 1 && "Invalid index");
      return mapValue(
          BV, InsertElementInst::Create(
                  transValue(CI->getComposite(), F, BB),
                  transValue(CI->getObject(), F, BB),
                  ConstantInt::get(*Context, APInt(32, CI->getIndices()[0])),
                  BV->getName(), BB));
    } else {
      auto CV = transValue(CI->getComposite(), F, BB);
      auto IndexedTy = ExtractValueInst::getIndexedType(
        CV->getType(), CI->getIndices());
      if (IndexedTy == nullptr) {
        // NOTE: "OpCompositeInsert" could insert a scalar component to a
        // vector or a vector in an aggregate. But in LLVM, "insertvalue" is
        // unable to do such thing. We have to replace it with "extractvalue" +
        // "insertelement" + "insertvalue" to achieve this purpose.
        assert(CI->getObject()->getType()->isTypeScalar());
        std::vector<SPIRVWord> Idxs = CI->getIndices();
        auto LastIdx = Idxs.back();
        Idxs.pop_back();

        Value* V = ExtractValueInst::Create(CV, Idxs, "", BB);
        assert(V->getType()->isVectorTy());
        V = InsertElementInst::Create(
                V, transValue(CI->getObject(), F, BB),
                ConstantInt::get(*Context, APInt(32, LastIdx)), "", BB);
        return mapValue(
            BV, InsertValueInst::Create(CV, widenBoolValue(V, BB),
              Idxs, BV->getName(), BB));
      } else
        return mapValue(
            BV, InsertValueInst::Create(
                    CV, widenBoolValue(transValue(CI->getObject(), F, BB), BB),
                    CI->getIndices(), BV->getName(), BB));
    }
  }

  case OpVectorInsertDynamic: {
    auto CI = static_cast<SPIRVVectorInsertDynamic *>(BV);
    return mapValue(
        BV, InsertElementInst::Create(transValue(CI->getVector(), F, BB),
                                      transValue(CI->getComponent(), F, BB),
                                      transValue(CI->getIndex(), F, BB),
                                      BV->getName(), BB));
  }

  case OpVectorShuffle: {
    // NOTE: LLVM backend compiler does not well handle "shufflevector"
    // instruction. So we avoid generating "shufflevector" and use the
    // combination of "extractelement" and "insertelement" as a substitute.
    auto VS = static_cast<SPIRVVectorShuffle *>(BV);

    auto V1 = transValue(VS->getVector1(), F, BB);
    auto V2 = transValue(VS->getVector2(), F, BB);

    auto Vec1CompCount = VS->getVector1ComponentCount();
    auto Vec2CompCount = VS->getVector2ComponentCount();
    auto NewVecCompCount = VS->getComponents().size();

    IntegerType *Int32Ty = IntegerType::get(*Context, 32);
    Type *NewVecTy   = VectorType::get(V1->getType()->getVectorElementType(),
                                       NewVecCompCount);
    Value *NewVec  = UndefValue::get(NewVecTy);

    for (size_t I = 0; I < NewVecCompCount; ++I) {
      auto Comp = VS->getComponents()[I];
      if (Comp < Vec1CompCount) {
        auto NewVecComp =
          ExtractElementInst::Create(V1,
                                     ConstantInt::get(Int32Ty, Comp),
                                     "", BB);
        NewVec  =
          InsertElementInst::Create(NewVec , NewVecComp,
                                    ConstantInt::get(Int32Ty, I),
                                    "", BB);
      } else {
        auto NewVecComp =
          ExtractElementInst::Create(V2,
                                    ConstantInt::get(Int32Ty,
                                                     Comp - Vec1CompCount),
                                    "", BB);

        NewVec  =
          InsertElementInst::Create(NewVec , NewVecComp,
                                    ConstantInt::get(Int32Ty, I),
                                    "", BB);
      }
    }

    return mapValue(BV, NewVec );
  }

  case OpFunctionCall: {
    SPIRVFunctionCall *BC = static_cast<SPIRVFunctionCall *>(BV);
    auto Call = CallInst::Create(transFunction(BC->getFunction()),
                                 transValue(BC->getArgumentValues(), F, BB),
                                 BC->getName(), BB);
    setCallingConv(Call);
    setAttrByCalledFunc(Call);
    return mapValue(BV, Call);
  }

  case OpExtInst: {
    SPIRVExtInst* BC = static_cast<SPIRVExtInst *>(BV);
    SPIRVExtInstSetKind Set = BM->getBuiltinSet(BC->getExtSetId());
    assert(Set == SPIRVEIS_OpenCL ||
           Set == SPIRVEIS_GLSL ||
           Set == SPIRVEIS_ShaderBallotAMD ||
           Set == SPIRVEIS_ShaderExplicitVertexParameterAMD ||
           Set == SPIRVEIS_GcnShaderAMD ||
           Set == SPIRVEIS_ShaderTrinaryMinMaxAMD);

    if (Set == SPIRVEIS_OpenCL)
      return mapValue(BV, transOCLBuiltinFromExtInst(BC, BB));
    else
      return mapValue(BV, transGLSLBuiltinFromExtInst(BC, BB));
  }

  case OpControlBarrier:
  case OpMemoryBarrier:
    return mapValue(
        BV, transOCLBarrierFence(static_cast<SPIRVInstruction *>(BV), BB));

  case OpSNegate: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    return mapValue(
        BV, BinaryOperator::CreateNSWNeg(transValue(BC->getOperand(0), F, BB),
                                         BV->getName(), BB));
  }
  case OpSMod: {
    return mapValue(BV, transBuiltinFromInst(
      "smod", static_cast<SPIRVInstruction *>(BV), BB));
  }
  case OpFMod: {
    // translate OpFMod(a, b) to copysign(frem(a, b), b)
    SPIRVFMod *FMod = static_cast<SPIRVFMod *>(BV);
    if (!IsKernel) {
      return mapValue(BV, transBuiltinFromInst(
        "fmod", static_cast<SPIRVInstruction *>(BV), BB));
    }
    auto Dividend = transValue(FMod->getDividend(), F, BB);
    auto Divisor = transValue(FMod->getDivisor(), F, BB);
    auto FRem = BinaryOperator::CreateFRem(Dividend, Divisor, "frem.res", BB);

    std::string UnmangledName = OCLExtOpMap::map(OpenCLLIB::Copysign);
    std::string MangledName = "copysign";

    std::vector<Type *> ArgTypes;
    ArgTypes.push_back(FRem->getType());
    ArgTypes.push_back(Divisor->getType());
    mangleOpenClBuiltin(UnmangledName, ArgTypes, MangledName);

    auto FT = FunctionType::get(transType(BV->getType()), ArgTypes, false);
    auto Func =
        Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);

    std::vector<Value *> Args;
    Args.push_back(FRem);
    Args.push_back(Divisor);

    auto Call = CallInst::Create(Func, Args, "copysign", BB);
    setCallingConv(Call);
    addFnAttr(Context, Call, Attribute::NoUnwind);
    return mapValue(BV, Call);
  }
  case OpFNegate: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    return mapValue(
        BV, BinaryOperator::CreateFNeg(transValue(BC->getOperand(0), F, BB),
                                       BV->getName(), BB));
  }
  case OpFDiv: {
    return mapValue(BV, transBuiltinFromInst(
      "fdiv", static_cast<SPIRVInstruction *>(BV), BB));
  }
  case OpQuantizeToF16: {
    return mapValue(BV, transBuiltinFromInst(
      "quantizeToF16", static_cast<SPIRVInstruction *>(BV), BB));
  }

  case OpLogicalNot:
  case OpNot: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    return mapValue(
        BV, BinaryOperator::CreateNot(transValue(BC->getOperand(0), F, BB),
                                      BV->getName(), BB));
  }

  case OpAll:
  case OpAny:
    return mapValue(BV,
                    transOCLAllAny(static_cast<SPIRVInstruction *>(BV), BB));

  case OpIsFinite:
  case OpIsInf:
  case OpIsNan:
  case OpIsNormal:
  case OpSignBitSet:
    return mapValue(
        BV, transOCLRelational(static_cast<SPIRVInstruction *>(BV), BB));

  case OpArrayLength: {
    SPIRVArrayLength *BI = static_cast<SPIRVArrayLength *>(BV);
    auto Struct = transValue(BI->getStruct(), F, BB);
    auto MemberIndex = ConstantInt::get(
      IntegerType::get(*Context, 32), BI->getMemberIndex());

    std::vector<Type *> ArgTys;
    ArgTys.push_back(Struct->getType());
    ArgTys.push_back(MemberIndex->getType());

    std::string MangledName;
    string OpName = getName(BI->getOpCode());
    mangleGlslBuiltin(OpName, ArgTys, MangledName);

    auto FuncTy = FunctionType::get(transType(BV->getType()), ArgTys, false);
    auto Func = Function::Create(FuncTy, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    Func->addFnAttr(Attribute::NoUnwind);

    std::vector<Value *> Args;
    Args.push_back(Struct);
    Args.push_back(MemberIndex);

    auto Call = CallInst::Create(Func, Args, "", BB);
    setCallingConv(Call);
    addFnAttr(Context, Call, Attribute::NoUnwind);

    return mapValue(BV, Call);
  }

  case OpImageSampleImplicitLod:
  case OpImageSampleExplicitLod:
  case OpImageSampleDrefImplicitLod:
  case OpImageSampleDrefExplicitLod:
  case OpImageSampleProjImplicitLod:
  case OpImageSampleProjExplicitLod:
  case OpImageSampleProjDrefImplicitLod:
  case OpImageSampleProjDrefExplicitLod:
  case OpImageFetch:
  case OpImageGather:
  case OpImageDrefGather:
  case OpImageQuerySizeLod:
  case OpImageQuerySize:
  case OpImageQueryLod:
  case OpImageQueryLevels:
  case OpImageQuerySamples:
  case OpImageRead:
  case OpImageWrite:
  case OpImageSparseSampleImplicitLod:
  case OpImageSparseSampleExplicitLod:
  case OpImageSparseSampleDrefImplicitLod:
  case OpImageSparseSampleDrefExplicitLod:
  case OpImageSparseSampleProjImplicitLod:
  case OpImageSparseSampleProjExplicitLod:
  case OpImageSparseSampleProjDrefImplicitLod:
  case OpImageSparseSampleProjDrefExplicitLod:
  case OpImageSparseFetch:
  case OpImageSparseGather:
  case OpImageSparseDrefGather:
  case OpImageSparseRead:
#if VKI_3RD_PARTY_IP_ANISOTROPIC_LOD_COMPENSATION
  case OpImageSampleAnisoLodAMD:
  case OpImageSampleDrefAnisoLodAMD:
  case OpImageGatherAnisoLodAMD:
  case OpImageDrefGatherAnisoLodAMD:
#endif
  {
    return mapValue(BV,
        transSPIRVImageOpFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));
  }
  case OpAtomicExchange:
  case OpAtomicCompareExchange:
  case OpAtomicIIncrement:
  case OpAtomicIDecrement:
  case OpAtomicIAdd:
  case OpAtomicISub:
  case OpAtomicSMin:
  case OpAtomicUMin:
  case OpAtomicSMax:
  case OpAtomicUMax:
  case OpAtomicAnd:
  case OpAtomicOr:
  case OpAtomicXor: {
    auto Pointer = static_cast<SPIRVInstruction *>(BV)->getOperands()[0];
    if (Pointer->getOpCode() == OpImageTexelPointer) {
      return mapValue(BV,
                      transSPIRVImageOpFromInst(
                        static_cast<SPIRVInstruction *>(BV),
                        BB));
    }
  }
  // For non-image atomic ops, fall through to atomic op common path
  case OpAtomicCompareExchangeWeak: {
    SPIRVInstruction *BI = static_cast<SPIRVInstruction *>(BV);
    string OpName = getName(BI->getOpCode());
    return mapValue(BV,
      transBuiltinFromInst(OpName, BI, BB));
  }
  case OpFragmentMaskFetchAMD:
  case OpFragmentFetchAMD:
    return mapValue(BV,
        transSPIRVFragmentMaskOpFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));
  case OpImageTexelPointer: {
    auto ImagePointer = static_cast<SPIRVImageTexelPointer *>(BV)->getImage();
    assert((ImagePointer->getOpCode() == OpAccessChain) ||
            ImagePointer->getOpCode() == OpVariable);
    LoadInst *LI = new LoadInst(transValue(ImagePointer, F, BB), BV->getName(),
        false, 0, BB);
    return mapValue(BV, LI);
  }
  case OpImageSparseTexelsResident: {
    SPIRVImageSparseTexelsResident *BI = static_cast<SPIRVImageSparseTexelsResident *>(BV);
    auto ResidentCode = transValue(BI->getResidentCode(), F, BB);

    std::string FuncName("llpc.imagesparse.texel.resident");
    SmallVector<Value *, 1> Arg;
    Arg.push_back(ResidentCode);

    Function *Func = M->getFunction(FuncName);
    if (!Func) {
      SmallVector<Type *, 1> ArgTy;
      ArgTy.push_back(Type::getInt32Ty(*Context));
      FunctionType *FuncTy = FunctionType::get(Type::getInt1Ty(*Context), ArgTy, false);
      Func = Function::Create(FuncTy, GlobalValue::ExternalLinkage, FuncName, M);
      Func->setCallingConv(CallingConv::SPIR_FUNC);
      if (isFuncNoUnwind())
        Func->addFnAttr(Attribute::NoUnwind);
    }

    return mapValue(BV, CallInst::Create(Func, Arg, "", BB));
  }
  default: {
    auto OC = BV->getOpCode();
    if (isSPIRVCmpInstTransToLLVMInst(static_cast<SPIRVInstruction *>(BV))) {
      return mapValue(BV, transCmpInst(BV, BB, F));
    } else if ((OCLSPIRVBuiltinMap::rfind(OC, nullptr) ||
                isIntelSubgroupOpCode(OC)) &&
               !isAtomicOpCode(OC) &&
               !isGroupOpCode(OC) &&
               !isPipeOpCode(OC) &&
               !isGroupNonUniformOpCode(OC )) {
      return mapValue(BV, transOCLBuiltinFromInst(
          static_cast<SPIRVInstruction *>(BV), BB));
    } else if (isBinaryShiftLogicalBitwiseOpCode(OC) ||
                isLogicalOpCode(OC)) {
      return mapValue(BV, transShiftLogicalBitwiseInst(BV, BB, F));
    } else if (isCvtOpCode(OC)) {
      auto BI = static_cast<SPIRVInstruction *>(BV);
      Value *Inst = nullptr;
      if (BI->hasFPRoundingMode() || BI->isSaturatedConversion())
        Inst = transOCLBuiltinFromInst(BI, BB);
      else
        Inst = transConvertInst(BV, F, BB);
      return mapValue(BV, Inst);
    }
    return mapValue(
        BV, transSPIRVBuiltinFromInst(static_cast<SPIRVInstruction *>(BV), BB));
  }

    SPIRVDBG(spvdbgs() << "Cannot translate " << *BV << '\n';)
    llvm_unreachable("Translation of SPIRV instruction not implemented");
    return NULL;
  }
}

void SPIRVToLLVM::truncConstantIndex(std::vector<Value*> &Indices, BasicBlock* BB) {
  // Only constant int32 can be used as struct index in LLVM
  // To simplify the logic, for constant index,
  // If constant is less than UINT32_MAX , translate all constant index to int32
  // Otherwise for non constant int, try convert them to int32
  for (uint32_t I = 0; I < Indices.size(); ++I) {
    auto Index = Indices[I];
    auto Int32Ty = Type::getInt32Ty(*Context);
    if (isa<ConstantInt>(Index)) {
      auto ConstIndex = cast<ConstantInt>(Index);
      if (ConstIndex->getType()->isIntegerTy(32) == false) {
        uint64_t ConstValue = ConstIndex->getZExtValue();
        if (ConstValue < UINT32_MAX) {
          auto ConstIndex32 = ConstantInt::get(Int32Ty, ConstValue);
          Indices[I] = ConstIndex32;
        }
      }
    } else {
      assert(isa<IntegerType>(Index->getType()));
      auto IndexTy = dyn_cast<IntegerType>(Index->getType());
      if(IndexTy->getBitWidth() < 32)
        // Convert 16 or 8 bit index to 32 bit integer
        Indices[I] = new ZExtInst(Index, Int32Ty, "", BB);
      else if(IndexTy->getBitWidth() > 32)
        // Convert 64 bit index to 32 bit integer
        Indices[I] = new TruncInst(Index, Int32Ty, "", BB);
    }
  }
}

template <class SourceTy, class FuncTy>
bool SPIRVToLLVM::foreachFuncCtlMask(SourceTy Source, FuncTy Func) {
  SPIRVWord FCM = Source->getFuncCtlMask();
  // Cancel those masks if they are both present
  if ((FCM & FunctionControlInlineMask) &&
      (FCM & FunctionControlDontInlineMask))
    FCM &= ~(FunctionControlInlineMask | FunctionControlDontInlineMask);
  SPIRSPIRVFuncCtlMaskMap::foreach([&](Attribute::AttrKind Attr,
      SPIRVFunctionControlMaskKind Mask){
    if (FCM & Mask)
      Func(Attr);
  });
  return true;
}

Function *SPIRVToLLVM::transFunction(SPIRVFunction *BF) {
  auto Loc = FuncMap.find(BF);
  if (Loc != FuncMap.end())
    return Loc->second;

  auto EntryPoint = BM->getEntryPoint(BF->getId());
  bool IsEntry = (EntryPoint != nullptr);
  SPIRVExecutionModelKind ExecModel =
    IsEntry ? EntryPoint->getExecModel() : ExecutionModelMax;
  auto Linkage = IsEntry ? GlobalValue::ExternalLinkage : transLinkageType(BF);
  FunctionType *FT = dyn_cast<FunctionType>(transType(BF->getFunctionType()));
  Function *F = dyn_cast<Function>(mapValue(BF, Function::Create(FT, Linkage,
      BF->getName(), M)));
  assert(F);
  mapFunction(BF, F);
  if (!F->isIntrinsic()) {
    if (IsEntry) {
      // Setup metadata for execution model
      std::vector<Metadata*> ExecModelMDs;
      auto Int32Ty = Type::getInt32Ty(*Context);
      ExecModelMDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, ExecModel)));
      auto ExecModelMDNode = MDNode::get(*Context, ExecModelMDs);
      F->addMetadata(gSPIRVMD::ExecutionModel, *ExecModelMDNode);
    }
    F->setCallingConv(CallingConv::SPIR_FUNC);

    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
    foreachFuncCtlMask(BF, [&](Attribute::AttrKind Attr){
      F->addFnAttr(Attr);
    });
  }

  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
       ++I) {
    auto BA = BF->getArgument(I->getArgNo());
    mapValue(BA, &(*I));
    setName(&(*I), BA);
    BA->foreachAttr([&](SPIRVFuncParamAttrKind Kind) {
      if (Kind == FunctionParameterAttributeNoWrite)
        return;
      F->addAttribute(I->getArgNo() + 1, SPIRSPIRVFuncParamAttrMap::rmap(Kind));
    });

    SPIRVWord MaxOffset = 0;
    if (BA->hasDecorate(DecorationMaxByteOffset, 0, &MaxOffset)) {
      AttrBuilder Builder;
      Builder.addDereferenceableAttr(MaxOffset);
      I->addAttrs(Builder);
    }
  }
  BF->foreachReturnValueAttr([&](SPIRVFuncParamAttrKind Kind) {
    if (Kind == FunctionParameterAttributeNoWrite)
      return;
    F->addAttribute(AttributeList::ReturnIndex,
                    SPIRSPIRVFuncParamAttrMap::rmap(Kind));
  });

  // Creating all basic blocks before creating instructions.
  for (size_t I = 0, E = BF->getNumBasicBlock(); I != E; ++I) {
    transValue(BF->getBasicBlock(I), F, nullptr);
  }

  // Set name for entry block
  if (F->getEntryBlock().getName().empty())
    F->getEntryBlock().setName(".entry");

  for (size_t I = 0, E = BF->getNumBasicBlock(); I != E; ++I) {
    SPIRVBasicBlock *BBB = BF->getBasicBlock(I);
    BasicBlock *BB = dyn_cast<BasicBlock>(transValue(BBB, F, nullptr));
    for (size_t BI = 0, BE = BBB->getNumInst(); BI != BE; ++BI) {
      SPIRVInstruction *BInst = BBB->getInst(BI);
      transValue(BInst, F, BB, false);
    }
  }
  return F;
}

/// LLVM convert builtin functions is translated to two instructions:
/// y = i32 islessgreater(float x, float z) ->
///     y = i32 ZExt(bool LessGreater(float x, float z))
/// When translating back, for simplicity, a trunc instruction is inserted
/// w = bool LessGreater(float x, float z) ->
///     w = bool Trunc(i32 islessgreater(float x, float z))
/// Optimizer should be able to remove the redundant trunc/zext
void SPIRVToLLVM::transOCLBuiltinFromInstPreproc(SPIRVInstruction *BI,
                                                 Type *&RetTy,
                                                 std::vector<Type*> &ArgTys,
                                                 std::vector<Value*> &Args,
                                                 BasicBlock *BB) {
  if (!BI->hasType())
    return;
  auto BT = BI->getType();
  auto OC = BI->getOpCode();
  if (isCmpOpCode(BI->getOpCode())) {
    if (BT->isTypeBool())
      RetTy = IntegerType::getInt32Ty(*Context);
    else if (BT->isTypeVectorBool())
      RetTy = VectorType::get(
          IntegerType::get(
              *Context, ArgTys[0]->getScalarSizeInBits() == 64 ? 64 : 32),
          BT->getVectorComponentCount());
    else
      llvm_unreachable("invalid compare instruction");
  } else if (OC == OpGenericCastToPtrExplicit) {
    Args.pop_back();
    ArgTys.pop_back();
  } else if (OC == OpImageRead && Args.size() > 2) {
    // Drop "Image operands" argument
    Args.erase(Args.begin() + 2);
    ArgTys.erase(ArgTys.begin() + 2);
  } else if (OC == OpBitFieldInsert || OC == OpBitFieldSExtract ||
             OC == OpBitFieldUExtract) {
    auto RetValBitWidth = RetTy->getScalarSizeInBits();
    if (ArgTys[2]->getScalarSizeInBits() != RetValBitWidth) {
      auto CastOp = ArgTys[2]->getScalarSizeInBits() > RetValBitWidth ?
          Instruction::Trunc : Instruction::ZExt;
      ArgTys[2] = IntegerType::getInt32Ty(*Context);
      Args[2] = CastInst::Create(CastOp, Args[2], ArgTys[2], "", BB);
    }
    auto Idx = (OC == OpBitFieldInsert) ? 3 : 1;
    if (ArgTys[Idx]->getScalarSizeInBits() != RetValBitWidth) {
      auto CastOp = ArgTys[Idx]->getScalarSizeInBits() > RetValBitWidth ?
          Instruction::Trunc : Instruction::ZExt;
      ArgTys[Idx] = IntegerType::getInt32Ty(*Context);
      Args[Idx] = CastInst::Create(CastOp, Args[Idx], ArgTys[Idx], "", BB);
    }
  }
}

Instruction *
SPIRVToLLVM::transOCLBuiltinPostproc(SPIRVInstruction *BI, CallInst *CI,
                                     BasicBlock *BB,
                                     const std::string &DemangledName) {
  auto OC = BI->getOpCode();
  if (isCmpOpCode(OC) && BI->getType()->isTypeVectorOrScalarBool()) {
    return CastInst::Create(Instruction::Trunc, CI, transType(BI->getType()),
                            "cvt", BB);
  }
  if (OC == OpImageSampleExplicitLod)
    return postProcessOCLReadImage(BI, CI, DemangledName);
  if (OC == OpImageWrite) {
    return postProcessOCLWriteImage(BI, CI, DemangledName);
  }
  if (OC == OpGenericPtrMemSemantics)
    return BinaryOperator::CreateShl(CI, getInt32(M, 8), "", BB);
  if (OC == OpImageQueryFormat)
    return BinaryOperator::CreateSub(
        CI, getInt32(M, OCLImageChannelDataTypeOffset), "", BB);
  if (OC == OpImageQueryOrder)
    return BinaryOperator::CreateSub(
        CI, getInt32(M, OCLImageChannelOrderOffset), "", BB);
  if (OC == OpBuildNDRange)
    return postProcessOCLBuildNDRange(BI, CI, DemangledName);
  if (OC == OpGroupAll || OC == OpGroupAny)
    return postProcessGroupAllAny(CI, DemangledName);
  if (SPIRVEnableStepExpansion &&
      (DemangledName == "smoothstep" || DemangledName == "step"))
    return expandOCLBuiltinWithScalarArg(CI, DemangledName);
  return CI;
}

static void adaptBlockInvoke(Function *Invoke, Type *BlockStructTy) {
  // As first argument block invoke takes a pointer to captured data.
  // We pass to block invoke whole block structure, not only captured data
  // as it expected. So we need to update original function to unpack expected
  // captured data and use it instead of an original argument
  //
  // %block = bitcast i8 addrspace(4)* to <{ ..., [X x i8] }> addrspace(4)*
  // %block.1 = addrspacecast %block to <{ ..., [X x i8] }>*
  // %captured = getelementptr <{ ..., [X x i8] }>, i32 0, i32 5
  // %captured.1 = bitcast %captured to i8*

  BasicBlock *BB = &(Invoke->getEntryBlock());
  BB->splitBasicBlock(BB->begin(), "invoke");
  auto FirstArg = &*(Invoke->arg_begin());
  IRBuilder<> Builder(BB, BB->begin());

  auto FirstArgTy = dyn_cast<PointerType>(FirstArg->getType());
  assert(FirstArgTy && "Expects that first argument of invoke is a pointer");
  unsigned FirstArgAS = FirstArgTy->getAddressSpace();

  auto Int8PtrTy =
      Type::getInt8PtrTy(Invoke->getParent()->getContext(), FirstArgAS);
  auto BlockStructPtrTy = PointerType::get(BlockStructTy, FirstArgAS);

  auto Int32Ty = Type::getInt32Ty(Invoke->getParent()->getContext());
  Value *CapturedGEPIndices[2] = {ConstantInt::get(Int32Ty, 0),
                                  ConstantInt::get(Int32Ty, 5)};
  auto BlockToStructCast =
      Builder.CreateBitCast(FirstArg, BlockStructPtrTy, "block");
  auto CapturedGEP = Builder.CreateGEP(BlockToStructCast, CapturedGEPIndices);
  auto CapturedToInt8Cast = Builder.CreateBitCast(CapturedGEP, Int8PtrTy);

  FirstArg->replaceUsesOutsideBlock(CapturedToInt8Cast, BB);
}

static Type *getOrCreateBlockDescTy(Module *M) {
  // Get or create block descriptor type which contains block size
  // in the last element:  %struct.__block_descriptor = type { i64, i64 }
  auto BlockDescTy = M->getTypeByName("struct.__block_descriptor");
  if (BlockDescTy)
    return BlockDescTy;

  auto Int64Ty = Type::getInt64Ty(M->getContext());
  Type *BlockDescElements[2] = {/*Reserved*/ Int64Ty, /*Block size*/ Int64Ty};
  return StructType::create(M->getContext(), BlockDescElements,
                            "struct.__block_descriptor");
}

Value *SPIRVToLLVM::transEnqueuedBlock(SPIRVValue *SInvoke,
                                       SPIRVValue *SCaptured,
                                       SPIRVValue *SCaptSize,
                                       SPIRVValue *SCaptAlignment,
                                       Function *LBI, BasicBlock *LBB) {
  // Search if that block have been already translated
  auto Loc = BlockMap.find(SInvoke);
  if (Loc != BlockMap.end())
    return Loc->second;

  IRBuilder<> Builder(LBB);
  const DataLayout &DL = M->getDataLayout();

  // Translate block and its arguments from SPIRV values to LLVM
  auto LInvoke = transFunction(static_cast<SPIRVFunction *>(SInvoke));
  auto LCaptured = transValue(SCaptured, LBI, LBB, false);
  auto LCaptSize =
      dyn_cast<ConstantInt>(transValue(SCaptSize, LBI, LBB, false));
  auto LCaptAlignment =
      dyn_cast<ConstantInt>(transValue(SCaptAlignment, LBI, LBB, false));

  // Create basic types
  auto Int8Ty = Type::getInt8Ty(*Context);
  auto Int32Ty = Type::getInt32Ty(*Context);
  auto Int8PtrTy = Type::getInt8PtrTy(*Context, SPIRAS_Private);
  auto Int8PtrTyGen = Type::getInt8PtrTy(*Context, SPIRAS_Generic);
  auto BlockDescTy = getOrCreateBlockDescTy(M);
  auto BlockDescPtrTy = BlockDescTy->getPointerTo(SPIRAS_Private);

  // Create a block as structure:
  // <{ i8*, i32, i32, i8*, %struct.__block_descriptor* }>
  SmallVector<Type *, 8> BlockEls = {
      /*isa*/ Int8PtrTy, /*flags*/ Int32Ty, /*reserved*/ Int32Ty,
      /*invoke*/ Int8PtrTy, /*block_descriptor*/ BlockDescPtrTy};

  // Add captured if any
  // <{ i8*, i32, i32, i8*, %struct.__block_descriptor*, [X x i8] }>
  // Note: captured data stored in structure as array of char
  if (LCaptSize->getZExtValue() > 0)
    BlockEls.push_back(ArrayType::get(Int8Ty, LCaptSize->getZExtValue()));

  auto BlockTy = StructType::get(*Context, BlockEls, /*isPacked*/ true);

  // Allocate block on the stack, then store data to it
  auto BlockAlloca = Builder.CreateAlloca(BlockTy, nullptr, "block");
  BlockAlloca->setAlignment(DL.getPrefTypeAlignment(BlockTy));

  auto GetIndices = [Int32Ty](int A, int B) -> SmallVector<Value *, 2> {
    return {ConstantInt::get(Int32Ty, A), ConstantInt::get(Int32Ty, B)};
  };

  // 1. isa, flags and reserved fields isn't used in current implementation
  // Fill them the same way as clang does
  auto IsaGEP = Builder.CreateGEP(BlockAlloca, GetIndices(0, 0));
  Builder.CreateStore(ConstantPointerNull::get(Int8PtrTy), IsaGEP);
  auto FlagsGEP = Builder.CreateGEP(BlockAlloca, GetIndices(0, 1));
  Builder.CreateStore(ConstantInt::get(Int32Ty, 1342177280), FlagsGEP);
  auto ReservedGEP = Builder.CreateGEP(BlockAlloca, GetIndices(0, 2));
  Builder.CreateStore(ConstantInt::get(Int32Ty, 0), ReservedGEP);

  // 2. Store pointer to block invoke to the structure
  auto InvokeCast = Builder.CreateBitCast(LInvoke, Int8PtrTy, "invoke");
  auto InvokeGEP = Builder.CreateGEP(BlockAlloca, GetIndices(0, 3));
  Builder.CreateStore(InvokeCast, InvokeGEP);

  // 3. Create and store a pointer to the block descriptor global value
  uint64_t SizeOfBlock = DL.getTypeAllocSize(BlockTy);

  auto Int64Ty = Type::getInt64Ty(*Context);
  Constant *BlockDescEls[2] = {ConstantInt::get(Int64Ty, 0),
                               ConstantInt::get(Int64Ty, SizeOfBlock)};
  auto BlockDesc =
      ConstantStruct::get(dyn_cast<StructType>(BlockDescTy), BlockDescEls);

  auto BlockDescGV =
      new GlobalVariable(*M, BlockDescTy, true, GlobalValue::InternalLinkage,
                         BlockDesc, "__block_descriptor_spirv");
  auto BlockDescGEP =
      Builder.CreateGEP(BlockAlloca, GetIndices(0, 4), "block.descriptor");
  Builder.CreateStore(BlockDescGV, BlockDescGEP);

  // 4. Copy captured data to the structure
  if (LCaptSize->getZExtValue() > 0) {
    auto CapturedGEP =
        Builder.CreateGEP(BlockAlloca, GetIndices(0, 5), "block.captured");
    auto CapturedGEPCast = Builder.CreateBitCast(CapturedGEP, Int8PtrTy);

    // We can't make any guesses about type of captured data, so
    // let's copy it through memcpy
    Builder.CreateMemCpy(CapturedGEPCast, LCaptAlignment->getZExtValue(),
                         LCaptured, LCaptAlignment->getZExtValue(), LCaptSize,
                         SCaptured->isVolatile());

    // Fix invoke function to correctly process its first argument
    adaptBlockInvoke(LInvoke, BlockTy);
  }
  auto BlockCast = Builder.CreateBitCast(BlockAlloca, Int8PtrTy);
  auto BlockCastGen = Builder.CreateAddrSpaceCast(BlockCast, Int8PtrTyGen);
  BlockMap[SInvoke] = BlockCastGen;
  return BlockCastGen;
}

Instruction *SPIRVToLLVM::transEnqueueKernelBI(SPIRVInstruction *BI,
                                               BasicBlock *BB) {
  Type *IntTy = Type::getInt32Ty(*Context);

  // Find or create enqueue kernel BI declaration
  auto Ops = BI->getOperands();
  bool HasVaargs = Ops.size() > 10;

  std::string FName = HasVaargs ? "__enqueue_kernel_events_vaargs"
                                : "__enqueue_kernel_basic_events";
  Function *F = M->getFunction(FName);
  if (!F) {
    Type *EventTy = PointerType::get(
        getOrCreateOpaquePtrType(M, SPIR_TYPE_NAME_CLK_EVENT_T, SPIRAS_Private),
        SPIRAS_Generic);

    SmallVector<Type *, 8> Tys = {
        transType(Ops[0]->getType()), // queue
        IntTy,                        // flags
        transType(Ops[2]->getType()), // ndrange
        IntTy,
        EventTy,
        EventTy,                                     // events
        Type::getInt8PtrTy(*Context, SPIRAS_Generic) // block
    };
    if (HasVaargs)
      Tys.push_back(IntTy); // Number of variadics if any

    FunctionType *FT = FunctionType::get(IntTy, Tys, HasVaargs);
    F = Function::Create(FT, GlobalValue::ExternalLinkage, FName, M);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
  }

  // Create call to enqueue kernel BI
  SmallVector<Value *, 8> Args = {
      transValue(Ops[0], F, BB, false), // queue
      transValue(Ops[1], F, BB, false), // flags
      transValue(Ops[2], F, BB, false), // ndrange
      transValue(Ops[3], F, BB, false), // events number
      transDeviceEvent(Ops[4], F, BB),  // event_wait_list
      transDeviceEvent(Ops[5], F, BB),  // event_ret
      transEnqueuedBlock(Ops[6], Ops[7], Ops[8], Ops[9], F, BB) // block
  };

  if (HasVaargs) {
    Args.push_back(
        ConstantInt::get(IntTy, Ops.size() - 10)); // Number of vaargs
    for (unsigned I = 10; I < Ops.size(); ++I)
      Args.push_back(transValue(Ops[I], F, BB, false));
  }
  auto Call = CallInst::Create(F, Args, "", BB);
  setName(Call, BI);
  setAttrByCalledFunc(Call);
  return Call;
}

Instruction *SPIRVToLLVM::transWGSizeBI(SPIRVInstruction *BI, BasicBlock *BB) {
  std::string FName = (BI->getOpCode() == OpGetKernelWorkGroupSize)
                          ? "__get_kernel_work_group_size_impl"
                          : "__get_kernel_preferred_work_group_multiple_impl";

  Function *F = M->getFunction(FName);
  if (!F) {
    auto Int8PtrTyGen = Type::getInt8PtrTy(*Context, SPIRAS_Generic);
    FunctionType *FT =
        FunctionType::get(Type::getInt32Ty(*Context), Int8PtrTyGen, false);
    F = Function::Create(FT, GlobalValue::ExternalLinkage, FName, M);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
  }
  auto Ops = BI->getOperands();
  auto Block = transEnqueuedBlock(Ops[0], Ops[1], Ops[2], Ops[3], F, BB);
  auto Call = CallInst::Create(F, Block, "", BB);
  setName(Call, BI);
  setAttrByCalledFunc(Call);
  return Call;
}

Instruction * SPIRVToLLVM::transBuiltinFromInst(const std::string& FuncName,
                                                SPIRVInstruction* BI,
                                                BasicBlock* BB) {
  std::string MangledName;
  auto Ops = BI->getOperands();
  auto RetBTy = BI->hasType() ? BI->getType() : nullptr;
  // NOTE: When function returns a structure-typed value,
  // we have to mark this structure type as "literal".
  if (BI->hasType() && RetBTy->getOpCode() == spv::OpTypeStruct) {
    auto StructType = static_cast<SPIRVTypeStruct *>(RetBTy);
    StructType->setLiteral(true);
  }
  Type* RetTy = BI->hasType() ? transType(RetBTy) :
      Type::getVoidTy(*Context);
  std::vector<Type*> ArgTys = transTypeVector(
      SPIRVInstruction::getOperandTypes(Ops));
  std::vector<Value*> Args = transValue(Ops, BB->getParent(), BB);
  transOCLBuiltinFromInstPreproc(BI, RetTy, ArgTys, Args, BB);
  bool HasFuncPtrArg = false;
  for (auto& I:ArgTys) {
    if (isa<FunctionType>(I)) {
      I = PointerType::get(I, SPIRAS_Private);
      HasFuncPtrArg = true;
    }
  }
  if (!IsKernel) {
    mangleGlslBuiltin(FuncName, ArgTys, MangledName);
  } else {
    if (!HasFuncPtrArg)
      mangleOpenClBuiltin(FuncName, ArgTys, MangledName);
    else
      MangledName = decorateSPIRVFunction(FuncName);
  }
  Function* Func = M->getFunction(MangledName);
  FunctionType* FT = FunctionType::get(RetTy, ArgTys, false);
  // ToDo: Some intermediate functions have duplicate names with
  // different function types. This is OK if the function name
  // is used internally and finally translated to unique function
  // names. However it is better to have a way to differentiate
  // between intermidiate functions and final functions and make
  // sure final functions have unique names.
  SPIRVDBG(
  if (!HasFuncPtrArg && Func && Func->getFunctionType() != FT) {
    dbgs() << "Warning: Function name conflict:\n"
       << *Func << '\n'
       << " => " << *FT << '\n';
  }
  )
  if (!Func || Func->getFunctionType() != FT) {
    SPIRVDBG(for (auto& I:ArgTys) {
      dbgs() << *I << '\n';
    });
    Func = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);
  }
  auto Call = CallInst::Create(Func, Args, "", BB);
  setName(Call, BI);
  setAttrByCalledFunc(Call);
  SPIRVDBG(spvdbgs() << "[transInstToBuiltinCall] " << *BI << " -> "; dbgs() <<
      *Call << '\n';)
  Instruction *Inst = Call;
  Inst = transOCLBuiltinPostproc(BI, Call, BB, FuncName);
  return Inst;
}

// Translates SPIR-V fragment mask operations to LLVM function calls
Instruction *
SPIRVToLLVM::transSPIRVFragmentMaskOpFromInst(SPIRVInstruction *BI,
                                              BasicBlock*BB) {
  Op OC = BI->getOpCode();

  const SPIRVTypeImageDescriptor *Desc = nullptr;
  std::vector<SPIRVValue *> Ops;
  std::vector<Type *> ArgTys;
  std::stringstream SS;

  // Generate name strings for image calls:
  // OpFragmentMaskFetchAMD:
  //    prefix.image.fetch.u32.dim.fmaskvalue
  // OpFragmentFetchAMD
  //    prefix.image.fetch.[f32|i32|u32].dim[.sample]

  // Add call prefix
  SS << gSPIRVName::ImageCallPrefix;
  SS << ".";

  // Add image operation kind
  std::string S;
  SPIRVImageOpKindNameMap::find(ImageOpFetch, &S);
  SS << S;

  // Collect operands
  Ops = BI->getOperands();
  std::vector<SPIRVType *> BTys = SPIRVInstruction::getOperandTypes(Ops);
  if (Ops[0]->getOpCode() == OpImageTexelPointer) {
      // Get image type from "ImageTexelPointer"
      BTys[0] = static_cast<SPIRVImageTexelPointer *>(Ops[0])->getImage()
      ->getType()->getPointerElementType();
  }
  ArgTys = transTypeVector(BTys);

  // Get image type info
  SPIRVType *BTy = BTys[0]; // Image operand
  if (BTy->isTypePointer())
    BTy = BTy->getPointerElementType();
  const SPIRVTypeImage *ImageTy = nullptr;

  OC = BTy->getOpCode();
  if (OC == OpTypeSampledImage) {
    ImageTy = static_cast<SPIRVTypeSampledImage *>(BTy)->getImageType();
    Desc    = &ImageTy->getDescriptor();
  } else if (OC == OpTypeImage) {
    ImageTy = static_cast<SPIRVTypeImage *>(BTy);
    Desc    = &ImageTy->getDescriptor();
  } else
    llvm_unreachable("Invalid image type");

  // Add sampled type
  if (BI->getOpCode() == OpFragmentMaskFetchAMD)
    SS << ".u32";
  else {
    SPIRVType *SampledTy = ImageTy->getSampledType();
    OC = SampledTy->getOpCode();
    if (OC == OpTypeFloat)
      SS << ".f32";
    else if (OC == OpTypeInt) {
      if (static_cast<SPIRVTypeInt*>(SampledTy)->isSigned())
        SS << ".i32";
      else
        SS << ".u32";
    } else
      llvm_unreachable("Invalid sampled type");
  }

  // Add image dimension
  assert((Desc->Dim == Dim2D) || (Desc->Dim == DimSubpassData));
  assert(Desc->MS);
  SS << "." << SPIRVDimNameMap::map(Desc->Dim);
  if (Desc->Arrayed)
      SS << "Array";

  if (BI->getOpCode() == OpFragmentMaskFetchAMD)
    SS << gSPIRVName::ImageCallModFmaskValue;
  else if (BI->getOpCode() == OpFragmentFetchAMD)
    SS << gSPIRVName::ImageCallModSample;

  std::vector<Value *> Args = transValue(Ops, BB->getParent(), BB);
  auto Int32Ty = Type::getInt32Ty(*Context);

  // Add image call metadata as argument
  ShaderImageCallMetadata ImageCallMD = {};
  ImageCallMD.OpKind        = ImageOpFetch;
  ImageCallMD.Dim           = Desc->Dim;
  ImageCallMD.Arrayed       = Desc->Arrayed;
  ImageCallMD.Multisampled  = Desc->MS;

  ArgTys.push_back(Int32Ty);
  Args.push_back(ConstantInt::get(Int32Ty, ImageCallMD.U32All));

  Function *F = M->getFunction(SS.str());
  Type *RetTy = Type::getVoidTy(*Context);
  assert(BI->hasType());
  RetTy = transType(BI->getType());
  FunctionType *FT = FunctionType::get(RetTy, ArgTys, false);

  if (!F) {
    F = Function::Create(FT, GlobalValue::ExternalLinkage, SS.str(), M);
    F->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
  }

  assert(F->getFunctionType() == FT);

  auto Call = CallInst::Create(F, Args, "", BB);
  setName(Call, BI);
  setAttrByCalledFunc(Call);

  return Call;
}

// Translates SPIR-V image operations to LLVM function calls
Value *
SPIRVToLLVM::transSPIRVImageOpFromInst(SPIRVInstruction *BI, BasicBlock*BB)
{
  Op OC = BI->getOpCode();
  SPIRVImageOpInfo Info;
  if (SPIRVImageOpInfoMap::find(OC, &Info) == false) {
    llvm_unreachable("Invalid image op code");
    return nullptr;
  }

  const SPIRVTypeImageDescriptor *Desc = nullptr;
  std::vector<SPIRVValue *> Ops;
  std::vector<Type *> ArgTys;
  std::stringstream SS;

  if (Info.OpKind != ImageOpQueryNonLod) {
    // Generate name strings for image calls:
    //    Format: prefix.image[sparse].op.[f32|i32|u32].dim[.proj][.dref][.lodnz][.bias][.lod][.grad]
    //                                                      [.constoffset][.offset]
    //                                                      [.constoffsets][.sample][.minlod]
#if VKI_3RD_PARTY_IP_ANISOTROPIC_LOD_COMPENSATION
    //                                                      [.anisolod]
#endif

    // Add call prefix
    SS << gSPIRVName::ImageCallPrefix;

    // Add sparse modifier
    if (Info.IsSparse)
      SS << gSPIRVName::ImageCallModSparse;

    SS << ".";

    // Add image operation kind
    std::string S;
    SPIRVImageOpKindNameMap::find(Info.OpKind, &S);
    SS << S;

    // Collect operands
    if (isImageAtomicOp(Info.OpKind)) {
        // NOTE: For atomic operations, extract image related info
        // from "ImageTexelPointer".
      SPIRVValue *ImagePointerOp =
        static_cast<SPIRVInstTemplateBase *>(BI)->getOperand(0);
      assert(ImagePointerOp->getOpCode() == OpImageTexelPointer);

      auto ImagePointer =
        static_cast<SPIRVImageTexelPointer *>(ImagePointerOp);
      SPIRVValue *Image = ImagePointer->getImage();
      assert((Image->getOpCode() == OpVariable) ||
          (Image->getOpCode() == OpAccessChain));
      assert(Image->getType()->isTypePointer());
      assert(Image->getType()->getPointerElementType()->isTypeImage());
      auto ImageTy = static_cast<SPIRVTypeImage *>(
          Image->getType()->getPointerElementType());
      Ops.push_back(ImagePointer);
      Ops.push_back(ImagePointer->getCoordinate());
      // Extract "sample" operand only if image is multi-sampled
      if (ImageTy->getDescriptor().MS != 0)
        Ops.push_back(ImagePointer->getSample());

      if (Info.OperAtomicData != InvalidOperIdx)
        Ops.push_back(static_cast<SPIRVInstTemplateBase *>(BI)->getOperand(
          Info.OperAtomicData));

      if (Info.OperAtomicComparator != InvalidOperIdx)
        Ops.push_back(static_cast<SPIRVInstTemplateBase *>(BI)->getOperand(
          Info.OperAtomicComparator));

    } else {
      // For other image operations, remove image operand mask and keep other operands
      for (size_t I = 0, E = BI->getOperands().size(); I != E; ++I) {
        if (I != Info.OperMask)
            Ops.push_back(
              static_cast<SPIRVInstTemplateBase *>(BI)->getOperand(I));
      }
    }

    std::vector<SPIRVType *> BTys = SPIRVInstruction::getOperandTypes(Ops);
    if (Ops[0]->getOpCode() == OpImageTexelPointer) {
        // Get image type from "ImageTexelPointer"
      BTys[0] = static_cast<SPIRVImageTexelPointer *>(Ops[0])->getImage()
        ->getType()->getPointerElementType();
    }
    ArgTys = transTypeVector(BTys);

    // Get image type info
    SPIRVType *BTy = BTys[0]; // Image operand
    if (BTy->isTypePointer())
      BTy = BTy->getPointerElementType();
    const SPIRVTypeImage *ImageTy = nullptr;

    Op TyOC = BTy->getOpCode();
    if (TyOC == OpTypeSampledImage) {
      ImageTy = static_cast<SPIRVTypeSampledImage *>(BTy)->getImageType();
      Desc    = &ImageTy->getDescriptor();
    } else if (TyOC == OpTypeImage) {
      ImageTy = static_cast<SPIRVTypeImage *>(BTy);
      Desc    = &ImageTy->getDescriptor();
    } else
      llvm_unreachable("Invalid image type");

    if (Info.OpKind == ImageOpQueryLod) {
      // Return type of "OpImageQueryLod" is always vec2
      SS << ".f32";
    } else {
      // Add sampled type
      SPIRVType *SampledTy = ImageTy->getSampledType();
      TyOC = SampledTy->getOpCode();
      if (TyOC == OpTypeFloat) {
        if (SampledTy->getBitWidth() == 16)
          SS << ".f16";
        else
          SS << ".f32";
      } else if (TyOC == OpTypeInt) {
        if (static_cast<SPIRVTypeInt*>(SampledTy)->isSigned())
          SS << ".i32";
        else
          SS << ".u32";
      } else
        llvm_unreachable("Invalid sampled type");
    }

    // Add image dimension
    SS << "." << SPIRVDimNameMap::map(Desc->Dim);
    if (Desc->Arrayed)
      SS << "Array";

    // NOTE: For "OpImageQueryLod", add "shadow" modifier to the call name.
    // It is only to keep function uniqueness (avoid overloading) and
    // will be removed in SPIR-V lowering.
    if (Info.OpKind == ImageOpQueryLod && Desc->Depth)
      SS << "Shadow";

    if (isImageAtomicOp(Info.OpKind) && Desc->MS) {
      assert(Desc->Dim == Dim2D);
      SS << gSPIRVName::ImageCallModSample;
    }

    if (Info.HasProj)
      SS << gSPIRVName::ImageCallModProj;

    if (Info.OperDref != InvalidOperIdx) {
      // Dref operand
      SS << gSPIRVName::ImageCallModDref;
    }

    SPIRVWord Mask = 0;
    auto &OpWords = static_cast<SPIRVInstTemplateBase *>(BI)->getOpWords();
    if (Info.OperMask < OpWords.size())
      // Optional image operands are present
      Mask = OpWords[Info.OperMask];

    // Lodnz for gather op
    if ((Info.OpKind == ImageOpGather) && EnableGatherLodNz) {
      if ((Mask & (ImageOperandsBiasMask |
                  ImageOperandsLodMask |
                  ImageOperandsGradMask |
                  ImageOperandsMinLodMask)) == 0)
          SS << gSPIRVName::ImageCallModLodNz;
    }

    // Bias operand
    if (Mask & ImageOperandsBiasMask)
      SS << gSPIRVName::ImageCallModBias;

    // Lod operand
    if (Mask & ImageOperandsLodMask)
      SS << gSPIRVName::ImageCallModLod;

    // Grad operands
    if (Mask & ImageOperandsGradMask)
      SS << gSPIRVName::ImageCallModGrad;

    // ConstOffset operands
    if (Mask & ImageOperandsConstOffsetMask)
      SS << gSPIRVName::ImageCallModConstOffset;

    // Offset operand
    if (Mask & ImageOperandsOffsetMask)
      SS << gSPIRVName::ImageCallModOffset;

    // ConstOffsets operand
    if (Mask & ImageOperandsConstOffsetsMask)
      SS << gSPIRVName::ImageCallModConstOffsets;

    // Sample operand
    if (Mask & ImageOperandsSampleMask)
      SS << gSPIRVName::ImageCallModSample;

    // MinLod operand
    if (Mask & ImageOperandsMinLodMask)
      SS << gSPIRVName::ImageCallModMinLod;

#if VKI_3RD_PARTY_IP_ANISOTROPIC_LOD_COMPENSATION
    if (isAnisoLodOpCode(OC))
      SS << gSPIRVName::ImageCallModAnisoLod;
#endif
    // Fmask usage is determined by resource node binding
    if (Desc->MS)
        SS << gSPIRVName::ImageCallModPatchFmaskUsage;

  } else {
    // Generate name strings for image query calls other than querylod
    Ops = BI->getOperands();
    assert(BI->hasType());
    std::vector<SPIRVType *> BTys = SPIRVInstruction::getOperandTypes(Ops);
    ArgTys = transTypeVector(BTys);

    // Get image type info
    assert(BTys[0]->getOpCode() == OpTypeImage);
    const SPIRVTypeImage *ImageBTy = static_cast<SPIRVTypeImage *>(BTys[0]);
    Desc    = &ImageBTy->getDescriptor();

    // Generate name strings for image query calls:
    //      Format: llpc.image.querynonlod.op.[dim][Array][.sample][.rettype]

    // Add call prefix
    SS << gSPIRVName::ImageCallPrefix << ".";

    // Add image operation kind: query
    std::string S;
    SPIRVImageOpKindNameMap::find(ImageOpQueryNonLod, &S);
    SS << S;

    // Add image query operation
    SPIRVImageQueryOpKindNameMap::find(OC, &S);
    SS << S;

    // Add image signature string to avoid overloading when image operand
    // have different type, it will be removed after image operand is lowered.
    StructType *ImageTy =
      dyn_cast<StructType>(dyn_cast<PointerType>(ArgTys[0])->getPointerElementType());
    StringRef ImageTyName = ImageTy->getName();
    StringRef DimName =
      ImageTyName.substr(ImageTyName.find_last_of('.'));
    SS << DimName.str();

    if ((OC == OpImageQuerySize) ||
        (OC == OpImageQuerySizeLod) ||
        (OC == OpImageQueryLevels)) {
      // Add image dimension info
      SPIRVImageDimKind dim = Desc->Dim;
      if (dim == DimRect)
          dim = Dim2D;

      SS << "." << SPIRVDimNameMap::map(dim);
      if (Desc->Arrayed)
        SS << "Array";
      if (Desc->MS)
        SS << gSPIRVName::ImageCallModSample;
    }

    if (OC == OpImageQuerySize || OC == OpImageQuerySizeLod) {
      // Add image query return type
      SPIRVType *RetBTy = BI->getType();
      uint32_t CompCount =
        RetBTy->isTypeVector() ? RetBTy->getVectorComponentCount() : 1;
      switch (CompCount) {
      case 1:
        assert((Desc->Dim == Dim1D) ||
               (Desc->Dim == DimBuffer));
        SS << ".i32";
        break;
      case 2:
        assert((Desc->Dim == Dim2D) ||
               (Desc->Dim == DimRect) ||
               (Desc->Dim == DimCube) ||
               (Desc->Arrayed && Desc->Dim == Dim1D));
        SS << ".v2i32";
        break;
      case 3:
        assert((Desc->Dim == Dim3D) ||
               (Desc->Arrayed && Desc->Dim == Dim2D) ||
               (Desc->Arrayed && Desc->Dim == DimCube));
        SS << ".v3i32";
        break;
      default:
        llvm_unreachable("Invalid return type");
        break;
      }
    }
  }

  std::vector<Value *> Args = transValue(Ops, BB->getParent(), BB);
  auto Int32Ty = Type::getInt32Ty(*Context);
  if (OC == OpImageQuerySize) {
    // Set LOD to zero
    ArgTys.push_back(Int32Ty);
    Args.push_back(ConstantInt::get(Int32Ty, 0));
  }

  // Add image call metadata as argument
  ShaderImageCallMetadata ImageCallMD = {};
  ImageCallMD.OpKind        = Info.OpKind;
  ImageCallMD.Dim           = Desc->Dim;
  ImageCallMD.Arrayed       = Desc->Arrayed;
  ImageCallMD.Multisampled  = Desc->MS;
  ArgTys.push_back(Int32Ty);
  Args.push_back(ConstantInt::get(Int32Ty, ImageCallMD.U32All));

  Function *F = M->getFunction(SS.str());
  Type *RetTy = Type::getVoidTy(*Context);
  if (Info.OpKind != ImageOpWrite) {
    assert(BI->hasType());
    RetTy = transType(BI->getType());
  }

  // For image read and image write, handle such case in which data argument
  // is not vec4.
  // NOTE: Such case is valid and can come from hand written or HLSL generated
  // SPIR-V shader
  uint32_t  DataCompCnt   = 4;
  if ((BI->getOpCode() == OpImageRead) ||
      ((BI->getOpCode() == OpImageFetch) && SPIRVWorkaroundBadSPIRV)) {
    DataCompCnt = (RetTy->isVectorTy() == false) ?
                    1 : RetTy->getVectorNumElements();
    assert(DataCompCnt <= 4);

    // For image read, need to change return type to vec4, and after
    // generating call to library function, need to change return
    // value from vec4 to the original type specified in SPIR-V.
    if (DataCompCnt != 4)
      RetTy = VectorType::get(RetTy->getScalarType(), 4);

  } else if (BI->getOpCode() == OpImageWrite) {
    Type  *DataTy = ArgTys[2];
    Value *Data   = Args[2];

    DataCompCnt = (DataTy->isVectorTy() == false) ?
                    1 : DataTy->getVectorNumElements();
    assert(DataCompCnt <= 4);

    if (DataCompCnt != 4) {
      // For image write, need to change data type to vec4, and zero-fill
      // the extra components.
      Type  *DataVec4Ty = VectorType::get(DataTy->getScalarType(), 4);
      Value *DataVec4 = nullptr;

      if (DataCompCnt == 1) {
        Value *DataZeroVec4 = ConstantAggregateZero::get(DataVec4Ty);
        DataVec4 = InsertElementInst::Create(DataZeroVec4,
                                             Data,
                                             ConstantInt::get(Type::getInt32Ty(*Context), 0),
                                             "",
                                             BB);
      } else {
        Value *DataZero = ConstantAggregateZero::get(DataTy);

        SmallVector<Constant*, 4> Idxs;
        for (uint32_t i = 0; i < 4; ++i)
          Idxs.push_back(ConstantInt::get(Type::getInt32Ty(*Context), i));

        Value *ShuffleMask = ConstantVector::get(Idxs);
        DataVec4 = new ShuffleVectorInst(Data, DataZero, ShuffleMask, "", BB);
      }

      ArgTys[2] = DataVec4Ty;
      Args[2] = DataVec4;
    }
  }

  FunctionType *FT = FunctionType::get(RetTy, ArgTys, false);

  if (!F) {
    F = Function::Create(FT, GlobalValue::ExternalLinkage, SS.str(), M);
    F->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
  }

  if (Info.OpKind != ImageOpQueryNonLod) {
    assert(F->getFunctionType() == FT);
  }

  CallInst *Call = CallInst::Create(F, Args, "", BB);
  setName(Call, BI);
  setAttrByCalledFunc(Call);

  // For image read, handle such case in which return value is not vec4
  // NOTE: Such case is valid and can come from hand written or HLSL generated
  // SPIR-V shader
  Value *RetVal = Call;
  if (((BI->getOpCode() == OpImageRead) ||
       ((BI->getOpCode() == OpImageFetch) && SPIRVWorkaroundBadSPIRV)) &&
      (DataCompCnt != 4)) {
    // Need to change return value of library function call from vec4 to the
    // original type specified in SPIR-V.
    assert(DataCompCnt < 4);

    if (DataCompCnt == 1)
      RetVal = ExtractElementInst::Create(Call,
                                          ConstantInt::get(
                                            Type::getInt32Ty(*Context), 0),
                                          "",
                                          BB);
    else {
      SmallVector<Constant*, 4> Idxs;
      for (uint32_t i = 0; i < DataCompCnt; ++i)
        Idxs.push_back(ConstantInt::get(Type::getInt32Ty(*Context), i));

      Value *ShuffleMask = ConstantVector::get(Idxs);
      RetVal = new ShuffleVectorInst(Call, Call, ShuffleMask, "", BB);
    }
  }

  return RetVal;
}

std::string SPIRVToLLVM::getOCLBuiltinName(SPIRVInstruction *BI) {
  auto OC = BI->getOpCode();
  if (OC == OpGenericCastToPtrExplicit)
    return getOCLGenericCastToPtrName(BI);
  if (isCvtOpCode(OC))
    return getOCLConvertBuiltinName(BI);
  if (OC == OpBuildNDRange) {
    auto NDRangeInst = static_cast<SPIRVBuildNDRange *>(BI);
    auto EleTy = ((NDRangeInst->getOperands())[0])->getType();
    int Dim = EleTy->isTypeArray() ? EleTy->getArrayLength() : 1;
    // cygwin does not have std::to_string
    ostringstream OS;
    OS << Dim;
    assert((EleTy->isTypeInt() && Dim == 1) ||
           (EleTy->isTypeArray() && Dim >= 2 && Dim <= 3));
    return std::string(kOCLBuiltinName::NDRangePrefix) + OS.str() + "D";
  }
  if (isIntelSubgroupOpCode(OC)) {
    std::stringstream Name;
    SPIRVType *DataTy = nullptr;
    switch (OC) {
    case OpSubgroupBlockReadINTEL:
    case OpSubgroupImageBlockReadINTEL:
      Name << "intel_sub_group_block_read";
      DataTy = BI->getType();
      break;
    case OpSubgroupBlockWriteINTEL:
      Name << "intel_sub_group_block_write";
      DataTy = BI->getOperands()[1]->getType();
      break;
    case OpSubgroupImageBlockWriteINTEL:
      Name << "intel_sub_group_block_write";
      DataTy = BI->getOperands()[2]->getType();
      break;
    default:
      return OCLSPIRVBuiltinMap::rmap(OC);
    }
    if (DataTy) {
      if (DataTy->getBitWidth() == 16)
        Name << "_us";
      if (DataTy->isTypeVector()) {
        if (unsigned ComponentCount = DataTy->getVectorComponentCount())
          Name << ComponentCount;
      }
    }
    return Name.str();
  }
  auto Name = OCLSPIRVBuiltinMap::rmap(OC);

  SPIRVType *T = nullptr;
  switch (OC) {
  case OpImageRead:
    T = BI->getType();
    break;
  case OpImageWrite:
    T = BI->getOperands()[2]->getType();
    break;
  default:
    // do nothing
    break;
  }
  if (T && T->isTypeVector())
    T = T->getVectorComponentType();
  if (T)
    Name += T->isTypeFloat()?'f':'i';

  return Name;
}

Instruction *SPIRVToLLVM::transOCLBuiltinFromInst(SPIRVInstruction *BI,
                                                  BasicBlock *BB) {
  assert(BB && "Invalid BB");
  auto FuncName = getOCLBuiltinName(BI);
  return transBuiltinFromInst(FuncName, BI, BB);
}

Instruction *SPIRVToLLVM::transSPIRVBuiltinFromInst(SPIRVInstruction *BI,
                                                    BasicBlock *BB) {
  assert(BB && "Invalid BB");
  string Suffix = "";
  if (BI->getOpCode() == OpCreatePipeFromPipeStorage) {
    auto CPFPS = static_cast<SPIRVCreatePipeFromPipeStorage *>(BI);
    assert(CPFPS->getType()->isTypePipe() &&
           "Invalid type of CreatePipeFromStorage");
    auto PipeType = static_cast<SPIRVTypePipe *>(CPFPS->getType());
    switch (PipeType->getAccessQualifier()) {
    default:
    case AccessQualifierReadOnly:
      Suffix = "_read";
      break;
    case AccessQualifierWriteOnly:
      Suffix = "_write";
      break;
    case AccessQualifierReadWrite:
      Suffix = "_read_write";
      break;
    }
  }

  if (!IsKernel)
    return transBuiltinFromInst(getName(BI->getOpCode()), BI, BB);
  else
    return transBuiltinFromInst(getSPIRVFuncName(BI->getOpCode(), Suffix), BI,
                                BB);
}

bool SPIRVToLLVM::translate(ExecutionModel EntryExecModel,
                            const char *EntryName) {
  if (!transAddressingModel())
    return false;

  // Find the targeted entry-point in this translation
  auto EntryPoint = BM->getEntryPoint(EntryExecModel, EntryName);
  if (EntryPoint == nullptr)
    return false;

  EntryTarget = BM->get<SPIRVFunction>(EntryPoint->getTargetId());
  if (EntryTarget == nullptr)
    return false;

  // Check if the SPIR-V corresponds to OpenCL kernel
  IsKernel = (EntryExecModel == ExecutionModelKernel);

  // Check if capability "VariablePointerStorageBuffer"  is enabled
  EnableVarPtr = BM->getCapability().find(
      CapabilityVariablePointersStorageBuffer) != BM->getCapability().end();
  EnableVarPtr = EnableVarPtr || (BM->getCapability().find(
      CapabilityVariablePointers) != BM->getCapability().end());

  EnableGatherLodNz = BM->hasCapability(CapabilityImageGatherBiasLodAMD) &&
      (EntryExecModel == ExecutionModelFragment);

  DbgTran.createCompileUnit();
  DbgTran.addDbgInfoVersion();

  for (unsigned I = 0, E = BM->getNumConstants(); I != E; ++I) {
    auto BV = BM->getConstant(I);
    auto OC = BV->getOpCode();
    if (OC == OpSpecConstant ||
        OC == OpSpecConstantTrue ||
        OC == OpSpecConstantFalse) {
      uint32_t SpecId = SPIRVID_INVALID;
      BV->hasDecorate(DecorationSpecId, 0, &SpecId);
      // assert(SpecId != SPIRVID_INVALID);
      if (SpecConstMap.find(SpecId) != SpecConstMap.end()) {
        auto SpecConstEntry = SpecConstMap.at(SpecId);
        assert(SpecConstEntry.DataSize <= sizeof(uint64_t));
        uint64_t Data = 0;
        memcpy(&Data, SpecConstEntry.Data, SpecConstEntry.DataSize);

        if (OC == OpSpecConstant)
          static_cast<SPIRVConstant *>(BV)->setZExtIntValue(Data);
        else if (OC == OpSpecConstantTrue)
          static_cast<SPIRVSpecConstantTrue *>(BV)->setBoolValue(Data != 0);
        else if (OC == OpSpecConstantFalse)
          static_cast<SPIRVSpecConstantFalse *>(BV)->setBoolValue(Data != 0);
        else
          llvm_unreachable("Invalid op code");
      }
    } else if (OC == OpSpecConstantOp) {
      if (!IsKernel) {
        // NOTE: Constant folding is applied to OpSpecConstantOp because at this
        // time, specialization info is obtained and all specialization constants
        // get their own finalized specialization values.
        auto BI = static_cast<SPIRVSpecConstantOp *>(BV);
        BV = createValueFromSpecConstantOp(BI);
        BI->mapToConstant(BV);
      }
    }
  }

  for (unsigned I = 0, E = BM->getNumVariables(); I != E; ++I) {
    auto BV = BM->getVariable(I);
    if (BV->getStorageClass() != StorageClassFunction)
      transValue(BV, nullptr, nullptr);
  }

  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    auto BF = BM->getFunction(I);
    // Non entry-points and targeted entry-point should be translated.
    // Set DLLExport on targeted entry-point so we can find it later.
    if (BM->getEntryPoint(BF->getId()) == nullptr || BF == EntryTarget) {
      auto F = transFunction(BF);
      if (BF == EntryTarget)
        F->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
    }
  }

  if (!transKernelMetadata())
    return false;
  if (!transFPContractMetadata())
    return false;
  if (!transSourceLanguage())
    return false;
  if (!transSourceExtension())
    return false;
  transGeneratorMD();

  if (IsKernel) {
    // NOTE: GLSL built-ins have been handled by transShaderDecoration(),
    // so we skip it here.
    if (!transOCLBuiltinsFromVariables())
      return false;
    // NOTE: OpenCL has made some changes for array and structure types after
    // SPIRV-to-LLVM translation. Such changes should not be applied to GLSL,
    // so skip them.
    if (!postProcessOCL())
      return false;
  }
  eraseUselessFunctions(M);
  DbgTran.finalize();
  return true;
}

bool SPIRVToLLVM::transAddressingModel() {
  switch (BM->getAddressingModel()) {
  case AddressingModelPhysical64:
    M->setTargetTriple(SPIR_TARGETTRIPLE64);
    M->setDataLayout(SPIR_DATALAYOUT64);
    break;
  case AddressingModelPhysical32:
    M->setTargetTriple(SPIR_TARGETTRIPLE32);
    M->setDataLayout(SPIR_DATALAYOUT32);
    break;
  case AddressingModelLogical:
    M->setTargetTriple(SPIR_TARGETTRIPLE64);
    M->setDataLayout(SPIR_DATALAYOUT64);
    break;
  default:
    SPIRVCKRT(0, InvalidAddressingModel,
              "Actual addressing mode is " +
                  std::to_string(BM->getAddressingModel()));
  }
  return true;
}

bool SPIRVToLLVM::transDecoration(SPIRVValue *BV, Value *V) {
  if (!transAlign(BV, V))
    return false;
  if (!transShaderDecoration(BV, V))
    return false;
  DbgTran.transDbgInfo(BV, V);
  return true;
}

bool SPIRVToLLVM::transFPContractMetadata() {
  bool ContractOff = false;
  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    SPIRVFunction *BF = BM->getFunction(I);
    if (!IsKernel)
      continue;
    if (BM->getEntryPoint(BF->getId()) != nullptr && BF != EntryTarget)
      continue; // Ignore those untargeted entry-points
    if (BF->getExecutionMode(ExecutionModeContractionOff)) {
      ContractOff = true;
      break;
    }
  }
  if (!ContractOff)
    M->getOrInsertNamedMetadata(kSPIR2MD::FPContract);
  return true;
}

std::string
SPIRVToLLVM::transOCLImageTypeAccessQualifier(SPIRV::SPIRVTypeImage *ST) {
  return SPIRSPIRVAccessQualifierMap::rmap(ST->hasAccessQualifier()
                                               ? ST->getAccessQualifier()
                                               : AccessQualifierReadOnly);
}

bool SPIRVToLLVM::transNonTemporalMetadata(Instruction *I) {
  Constant *One = ConstantInt::get(Type::getInt32Ty(*Context), 1);
  MDNode *Node = MDNode::get(*Context, ConstantAsMetadata::get(One));
  I->setMetadata(M->getMDKindID("nontemporal"), Node);
  return true;
}

bool SPIRVToLLVM::transKernelMetadata() {
  NamedMDNode *KernelMDs = M->getOrInsertNamedMetadata(SPIR_MD_KERNELS);
  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    SPIRVFunction *BF = BM->getFunction(I);
    auto EntryPoint = BM->getEntryPoint(BF->getId());
    if (EntryPoint != nullptr && BF != EntryTarget)
      continue; // Ignore those untargeted entry-points

    Function *F = static_cast<Function *>(getTranslatedValue(BF));
    assert(F && "Invalid translated function");

    if (EntryPoint == nullptr)
      continue;
    SPIRVExecutionModelKind ExecModel = EntryPoint->getExecModel();

    if (ExecModel != ExecutionModelKernel) {
      NamedMDNode *EntryMDs =
          M->getOrInsertNamedMetadata(gSPIRVMD::EntryPoints);
      std::vector<llvm::Metadata *> EntryMD;
      EntryMD.push_back(ValueAsMetadata::get(F));

      // Generate metadata for execution modes
      ShaderExecModeMetadata ExecModeMD = {};

      if (ExecModel == ExecutionModelVertex) {
        if (BF->getExecutionMode(ExecutionModeXfb))
          ExecModeMD.vs.xfb = true;

      } else if (ExecModel == ExecutionModelTessellationControl ||
                 ExecModel == ExecutionModelTessellationEvaluation) {
        if (BF->getExecutionMode(ExecutionModeSpacingEqual))
          ExecModeMD.ts.SpacingEqual = true;
        if (BF->getExecutionMode(ExecutionModeSpacingFractionalEven))
          ExecModeMD.ts.SpacingFractionalEven = true;
        if (BF->getExecutionMode(ExecutionModeSpacingFractionalOdd))
          ExecModeMD.ts.SpacingFractionalOdd = true;

        if (BF->getExecutionMode(ExecutionModeVertexOrderCw))
          ExecModeMD.ts.VertexOrderCw = true;
        if (BF->getExecutionMode(ExecutionModeVertexOrderCcw))
          ExecModeMD.ts.VertexOrderCcw = true;

        if (BF->getExecutionMode(ExecutionModePointMode))
          ExecModeMD.ts.PointMode = true;

        if (BF->getExecutionMode(ExecutionModeTriangles))
          ExecModeMD.ts.Triangles = true;
        if (BF->getExecutionMode(ExecutionModeQuads))
          ExecModeMD.ts.Quads = true;
        if (BF->getExecutionMode(ExecutionModeIsolines))
          ExecModeMD.ts.Isolines = true;

        if (BF->getExecutionMode(ExecutionModeXfb))
          ExecModeMD.ts.xfb = true;

        if (auto EM = BF->getExecutionMode(ExecutionModeOutputVertices))
          ExecModeMD.ts.OutputVertices = EM->getLiterals()[0];

      } else if (ExecModel == ExecutionModelGeometry) {
        if (BF->getExecutionMode(ExecutionModeInputPoints))
          ExecModeMD.gs.InputPoints = true;
        if (BF->getExecutionMode(ExecutionModeInputLines))
          ExecModeMD.gs.InputLines = true;
        if (BF->getExecutionMode(ExecutionModeInputLinesAdjacency))
          ExecModeMD.gs.InputLinesAdjacency = true;
        if (BF->getExecutionMode(ExecutionModeTriangles))
          ExecModeMD.gs.Triangles = true;
        if (BF->getExecutionMode(ExecutionModeInputTrianglesAdjacency))
          ExecModeMD.gs.InputTrianglesAdjacency = true;

        if (BF->getExecutionMode(ExecutionModeOutputPoints))
          ExecModeMD.gs.OutputPoints = true;
        if (BF->getExecutionMode(ExecutionModeOutputLineStrip))
          ExecModeMD.gs.OutputLineStrip = true;
        if (BF->getExecutionMode(ExecutionModeOutputTriangleStrip))
          ExecModeMD.gs.OutputTriangleStrip = true;

        if (BF->getExecutionMode(ExecutionModeXfb))
          ExecModeMD.gs.xfb = true;

        if (auto EM = BF->getExecutionMode(ExecutionModeInvocations))
          ExecModeMD.gs.Invocations = EM->getLiterals()[0];

        if (auto EM = BF->getExecutionMode(ExecutionModeOutputVertices))
          ExecModeMD.gs.OutputVertices = EM->getLiterals()[0];

      } else if (ExecModel == ExecutionModelFragment) {
        if (BF->getExecutionMode(ExecutionModeOriginUpperLeft))
          ExecModeMD.fs.OriginUpperLeft = true;
        else if (BF->getExecutionMode(ExecutionModeOriginLowerLeft))
          ExecModeMD.fs.OriginUpperLeft = false;

        if (BF->getExecutionMode(ExecutionModePixelCenterInteger))
          ExecModeMD.fs.PixelCenterInteger = true;

        if (BF->getExecutionMode(ExecutionModeEarlyFragmentTests))
          ExecModeMD.fs.EarlyFragmentTests = true;

        if (BF->getExecutionMode(ExecutionModeDepthUnchanged))
          ExecModeMD.fs.DepthUnchanged = true;
        if (BF->getExecutionMode(ExecutionModeDepthGreater))
          ExecModeMD.fs.DepthGreater = true;
        if (BF->getExecutionMode(ExecutionModeDepthLess))
          ExecModeMD.fs.DepthLess = true;
        if (BF->getExecutionMode(ExecutionModeDepthReplacing))
          ExecModeMD.fs.DepthReplacing = true;

      } else if (ExecModel == ExecutionModelGLCompute) {
        // Set values of local sizes from execution model
        if (auto EM = BF->getExecutionMode(ExecutionModeLocalSize)) {
          ExecModeMD.cs.LocalSizeX = EM->getLiterals()[0];
          ExecModeMD.cs.LocalSizeY = EM->getLiterals()[1];
          ExecModeMD.cs.LocalSizeZ = EM->getLiterals()[2];
        }

        // Traverse the constant list to find gl_WorkGroupSize and use the
        // values to overwrite local sizes
        for (unsigned I = 0, E = BM->getNumConstants(); I != E; ++I) {
          auto BV = BM->getConstant(I);
          SPIRVWord BuiltIn = SPIRVID_INVALID;
          if ((BV->getOpCode() == OpSpecConstant ||
                BV->getOpCode() == OpSpecConstantComposite) &&
              BV->hasDecorate(DecorationBuiltIn, 0, &BuiltIn)) {
            if (BuiltIn == BuiltInWorkgroupSize) {
              // NOTE: Overwrite values of local sizes specified in execution
              // mode if the constant corresponding to gl_WorkGroupSize
              // exists. Take its value since gl_WorkGroupSize could be a
              // specialization constant.
              auto WorkGroupSize =
                static_cast<SPIRVSpecConstantComposite *>(BV);

              // Declared: const uvec3 gl_WorkGroupSize
              assert(WorkGroupSize->getElements().size() == 3);
              auto WorkGroupSizeX =
                static_cast<SPIRVConstant *>(WorkGroupSize->getElements()[0]);
              auto WorkGroupSizeY =
                static_cast<SPIRVConstant *>(WorkGroupSize->getElements()[1]);
              auto WorkGroupSizeZ =
                static_cast<SPIRVConstant *>(WorkGroupSize->getElements()[2]);

              ExecModeMD.cs.LocalSizeX = WorkGroupSizeX->getZExtIntValue();
              ExecModeMD.cs.LocalSizeY = WorkGroupSizeY->getZExtIntValue();
              ExecModeMD.cs.LocalSizeZ = WorkGroupSizeZ->getZExtIntValue();

              break;
            }
          }
        }
      } else
        llvm_unreachable("Invalid execution model");

      static_assert(sizeof(ExecModeMD) == 3 * sizeof(uint32_t),
          "Unexpected size");
      std::vector<uint32_t> MDVec;
      MDVec.push_back(ExecModeMD.U32All[0]);
      MDVec.push_back(ExecModeMD.U32All[1]);
      MDVec.push_back(ExecModeMD.U32All[2]);

      EntryMD.push_back(getMDNodeStringIntVec(Context,
          gSPIRVMD::ExecutionMode + std::string(".") + getName(ExecModel),
          MDVec));

      MDNode *MDNode = MDNode::get(*Context, EntryMD);
      EntryMDs->addOperand(MDNode);

      // Skip the following processing for GLSL
      continue;
    }

    // Generate metadata for kernel_arg_address_spaces
    addOCLKernelArgumentMetadata(
        Context, SPIR_MD_KERNEL_ARG_ADDR_SPACE, BF, F,
        [=](SPIRVFunctionParameter *Arg) {
          SPIRVType *ArgTy = Arg->getType();
          SPIRAddressSpace AS = SPIRAS_Private;
          if (ArgTy->isTypePointer())
            AS = SPIRSPIRVAddrSpaceMap::rmap(ArgTy->getPointerStorageClass());
          else if (ArgTy->isTypeOCLImage() || ArgTy->isTypePipe())
            AS = SPIRAS_Global;
          return ConstantAsMetadata::get(
              ConstantInt::get(Type::getInt32Ty(*Context), AS));
        });
    // Generate metadata for kernel_arg_access_qual
    addOCLKernelArgumentMetadata(Context, SPIR_MD_KERNEL_ARG_ACCESS_QUAL, BF, F,
                                 [=](SPIRVFunctionParameter *Arg) {
                                   std::string Qual;
                                   auto T = Arg->getType();
                                   if (T->isTypeOCLImage()) {
                                     auto ST = static_cast<SPIRVTypeImage *>(T);
                                     Qual =
                                         transOCLImageTypeAccessQualifier(ST);
                                   } else if (T->isTypePipe()) {
                                     auto PT = static_cast<SPIRVTypePipe *>(T);
                                     Qual = transOCLPipeTypeAccessQualifier(PT);
                                   } else
                                     Qual = "none";
                                   return MDString::get(*Context, Qual);
                                 });
    // Generate metadata for kernel_arg_type
    addOCLKernelArgumentMetadata(Context, SPIR_MD_KERNEL_ARG_TYPE, BF, F,
                                 [=](SPIRVFunctionParameter *Arg) {
                                   return transOCLKernelArgTypeName(Arg);
                                 });
    // Generate metadata for kernel_arg_type_qual
    addOCLKernelArgumentMetadata(
        Context, SPIR_MD_KERNEL_ARG_TYPE_QUAL, BF, F,
        [=](SPIRVFunctionParameter *Arg) {
          std::string Qual;
          if (Arg->hasDecorate(DecorationVolatile))
            Qual = kOCLTypeQualifierName::Volatile;
          Arg->foreachAttr([&](SPIRVFuncParamAttrKind Kind) {
            Qual += Qual.empty() ? "" : " ";
            switch (Kind) {
            case FunctionParameterAttributeNoAlias:
              Qual += kOCLTypeQualifierName::Restrict;
              break;
            case FunctionParameterAttributeNoWrite:
              Qual += kOCLTypeQualifierName::Const;
              break;
            default:
              // do nothing.
              break;
            }
          });
          if (Arg->getType()->isTypePipe()) {
            Qual += Qual.empty() ? "" : " ";
            Qual += kOCLTypeQualifierName::Pipe;
          }
          return MDString::get(*Context, Qual);
        });
    // Generate metadata for kernel_arg_base_type
    addOCLKernelArgumentMetadata(Context, SPIR_MD_KERNEL_ARG_BASE_TYPE, BF, F,
                                 [=](SPIRVFunctionParameter *Arg) {
                                   return transOCLKernelArgTypeName(Arg);
                                 });
    // Generate metadata for kernel_arg_name
    if (SPIRVGenKernelArgNameMD) {
      bool ArgHasName = true;
      BF->foreachArgument([&](SPIRVFunctionParameter *Arg) {
        ArgHasName &= !Arg->getName().empty();
      });
      if (ArgHasName)
        addOCLKernelArgumentMetadata(Context, SPIR_MD_KERNEL_ARG_NAME, BF, F,
                                     [=](SPIRVFunctionParameter *Arg) {
                                       return MDString::get(*Context,
                                                            Arg->getName());
                                     });
    }
    // Generate metadata for reqd_work_group_size
    if (auto EM = BF->getExecutionMode(ExecutionModeLocalSize)) {
      F->setMetadata(kSPIR2MD::WGSize,
                     getMDNodeStringIntVec(Context, EM->getLiterals()));
    }
    // Generate metadata for work_group_size_hint
    if (auto EM = BF->getExecutionMode(ExecutionModeLocalSizeHint)) {
      F->setMetadata(kSPIR2MD::WGSizeHint,
                     getMDNodeStringIntVec(Context, EM->getLiterals()));
    }
    // Generate metadata for vec_type_hint
    if (auto EM = BF->getExecutionMode(ExecutionModeVecTypeHint)) {
      std::vector<Metadata *> MetadataVec;
      Type *VecHintTy = decodeVecTypeHint(*Context, EM->getLiterals()[0]);
      assert(VecHintTy);
      MetadataVec.push_back(ValueAsMetadata::get(UndefValue::get(VecHintTy)));
      MetadataVec.push_back(ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(*Context), 1)));
      F->setMetadata(kSPIR2MD::VecTyHint, MDNode::get(*Context, MetadataVec));
    }
  }
  return true;
}

bool SPIRVToLLVM::transAlign(SPIRVValue *BV, Value *V) {
  if (auto AL = dyn_cast<AllocaInst>(V)) {
    SPIRVWord Align = 0;
    if (BV->hasAlignment(&Align))
      AL->setAlignment(Align);
    return true;
  }
  if (auto GV = dyn_cast<GlobalVariable>(V)) {
    SPIRVWord Align = 0;
    if (BV->hasAlignment(&Align))
      GV->setAlignment(Align);
    return true;
  }
  return true;
}

bool SPIRVToLLVM::transShaderDecoration(SPIRVValue *BV, Value *V) {
  auto GV = dyn_cast<GlobalVariable>(V);
  if (GV) {
    auto AS = GV->getType()->getAddressSpace();
    if (AS == SPIRAS_Input || AS == SPIRAS_Output) {
      // Translate decorations of inputs and outputs

      // Build input/output metadata
      ShaderInOutDecorate InOutDec = {};
      InOutDec.Value.U32All = 0;
      InOutDec.IsBuiltIn = false;
      InOutDec.Interp.Mode = InterpModeSmooth;
      InOutDec.Interp.Loc = InterpLocCenter;
      InOutDec.PerPatch = false;
      InOutDec.StreamId = 0;
      InOutDec.Index = 0;

      SPIRVWord Loc = SPIRVID_INVALID;
      if (BV->hasDecorate(DecorationLocation, 0, &Loc)) {
        InOutDec.IsBuiltIn = false;
        InOutDec.Value.Loc = Loc;
      }

      SPIRVWord Index = SPIRVID_INVALID;
      if (BV->hasDecorate(DecorationIndex, 0, &Index)) {
          InOutDec.Index = Index;
      }

      SPIRVWord BuiltIn = SPIRVID_INVALID;
      if (BV->hasDecorate(DecorationBuiltIn, 0, &BuiltIn)) {
        InOutDec.IsBuiltIn = true;
        InOutDec.Value.BuiltIn = BuiltIn;
      } else if (BV->getName() == "gl_in" || BV->getName() == "gl_out") {
        InOutDec.IsBuiltIn = true;
        InOutDec.Value.BuiltIn = BuiltInPerVertex;
      }

      SPIRVWord Component = SPIRVID_INVALID;
      if (BV->hasDecorate(DecorationComponent, 0, &Component))
        InOutDec.Component = Component;

      if (BV->hasDecorate(DecorationFlat))
        InOutDec.Interp.Mode = InterpModeFlat;

      if (BV->hasDecorate(DecorationNoPerspective))
        InOutDec.Interp.Mode = InterpModeNoPersp;

      if (BV->hasDecorate(DecorationCentroid))
        InOutDec.Interp.Loc = InterpLocCentroid;

      if (BV->hasDecorate(DecorationSample))
        InOutDec.Interp.Loc = InterpLocSample;

      if (BV->hasDecorate(DecorationExplicitInterpAMD)) {
        InOutDec.Interp.Mode = InterpModeCustom;
        InOutDec.Interp.Loc = InterpLocCustom;
      }

      if (BV->hasDecorate(DecorationPatch))
        InOutDec.PerPatch = true;

      SPIRVWord StreamId = SPIRVID_INVALID;
      if (BV->hasDecorate(DecorationStream, 0, &StreamId))
        InOutDec.StreamId = StreamId;

      Type* MDTy = nullptr;
      SPIRVType* BT = BV->getType()->getPointerElementType();
      auto MD = buildShaderInOutMetadata(BT, InOutDec, MDTy);

      // Setup input/output metadata
      std::vector<Metadata*> MDs;
      MDs.push_back(ConstantAsMetadata::get(MD));
      auto MDNode = MDNode::get(*Context, MDs);
      GV->addMetadata(gSPIRVMD::InOut, *MDNode);

    } else if (AS == SPIRAS_Uniform) {
      // Translate decorations of blocks
      // Remove array dimensions, it is useless for block metadata building
      SPIRVType* BlockTy = nullptr;
      if (EnableVarPtr &&
        (BV->getType()->getPointerStorageClass() == StorageClassStorageBuffer))
        // NOTE: Keep the pointer type for variable pointer storage buffer.
        BlockTy = BV->getType();
      else {
        BlockTy = BV->getType()->getPointerElementType();
        while (BlockTy->isTypeArray())
          BlockTy = BlockTy->getArrayElementType();
        assert(BlockTy->isTypeStruct());
      }

      // Get values of descriptor binding and set based on corresponding
      // decorations
      SPIRVWord Binding = SPIRVID_INVALID;
      SPIRVWord DescSet = SPIRVID_INVALID;
      bool HasBinding = BV->hasDecorate(DecorationBinding, 0, &Binding);
      bool HasDescSet = BV->hasDecorate(DecorationDescriptorSet, 0, &DescSet);

      // TODO: Currently, set default binding and descriptor to 0. Will be
      // changed later.
      if (HasBinding == false)
        Binding = 0;
      if (HasDescSet == false)
        DescSet = 0;

      // Determine block type based on corresponding decorations
      SPIRVBlockTypeKind BlockType = BlockTypeUnknown;

      if (BV->getType()->getPointerStorageClass() == StorageClassStorageBuffer)
        BlockType = BlockTypeShaderStorage;
      else {
        bool IsUniformBlock = BlockTy->hasDecorate(DecorationBlock);
        bool IsStorageBlock = BlockTy->hasDecorate(DecorationBufferBlock);
          if (IsUniformBlock)
            BlockType = BlockTypeUniform;
          else if (IsStorageBlock)
            BlockType = BlockTypeShaderStorage;
      }
      // Setup resource metadata
      auto Int32Ty = Type::getInt32Ty(*Context);
      std::vector<Metadata*> ResMDs;
      ResMDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, DescSet)));
      ResMDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, Binding)));
      ResMDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, BlockType)));
      auto ResMDNode = MDNode::get(*Context, ResMDs);
      GV->addMetadata(gSPIRVMD::Resource, *ResMDNode);

      // Build block metadata
      ShaderBlockDecorate BlockDec = {};
      Type *BlockMDTy = nullptr;
      auto BlockMD = buildShaderBlockMetadata(BlockTy, BlockDec, BlockMDTy);

      std::vector<Metadata*> BlockMDs;
      BlockMDs.push_back(ConstantAsMetadata::get(BlockMD));
      auto BlockMDNode = MDNode::get(*Context, BlockMDs);
      GV->addMetadata(gSPIRVMD::Block, *BlockMDNode);

    } else if (AS == SPIRAS_PushConst) {
      // Translate decorations of push constants

      SPIRVType *PushConstTy = BV->getType()->getPointerElementType();
      assert(PushConstTy->isTypeStruct());

      // Build push constant specific metadata
      uint32_t PushConstSize = 0;
      uint32_t MatrixStride = SPIRVID_INVALID;
      bool IsRowMajor = false;
      PushConstSize = calcShaderBlockSize(
        PushConstTy, PushConstSize, MatrixStride, IsRowMajor);

      auto Int32Ty = Type::getInt32Ty(*Context);
      std::vector<Metadata *> PushConstMDs;
      PushConstMDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, PushConstSize)));
      auto PushConstMDNode = MDNode::get(*Context, PushConstMDs);
      GV->addMetadata(gSPIRVMD::PushConst, *PushConstMDNode);

      // Build general block metadata
      ShaderBlockDecorate BlockDec = {};
      Type *BlockMDTy = nullptr;
      auto BlockMD = buildShaderBlockMetadata(
        PushConstTy, BlockDec, BlockMDTy);

      std::vector<Metadata *> BlockMDs;
      BlockMDs.push_back(ConstantAsMetadata::get(BlockMD));
      auto BlockMDNode = MDNode::get(*Context, BlockMDs);
      GV->addMetadata(gSPIRVMD::Block, *BlockMDNode);

    } else if (AS == SPIRAS_Constant) {
      // Translate decorations of uniform constants (images or samplers)

      SPIRVType *OpaqueTy = BV->getType()->getPointerElementType();
      while (OpaqueTy->isTypeArray())
        OpaqueTy = OpaqueTy->getArrayElementType();
      assert(OpaqueTy->isTypeImage() ||
             OpaqueTy->isTypeSampledImage() ||
             OpaqueTy->isTypeSampler());

      // Get values of descriptor binding and set based on corresponding
      // decorations
      SPIRVWord DescSet = SPIRVID_INVALID;
      SPIRVWord Binding = SPIRVID_INVALID;
      bool HasBinding = BV->hasDecorate(DecorationBinding, 0, &Binding);
      bool HasDescSet = BV->hasDecorate(DecorationDescriptorSet, 0, &DescSet);

      // TODO: Currently, set default binding and descriptor to 0. Will be
      // changed later.
      if (HasBinding == false)
          Binding = 0;
      if (HasDescSet == false)
          DescSet = 0;

      // Setup resource metadata
      auto Int32Ty = Type::getInt32Ty(*Context);
      std::vector<Metadata*> MDs;
      MDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, DescSet)));
      MDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, Binding)));
      auto MDNode = MDNode::get(*Context, MDs);
      GV->addMetadata(gSPIRVMD::Resource, *MDNode);

      // Build image memory metadata
      if (OpaqueTy->isTypeImage()) {
        auto ImageTy = static_cast<SPIRVTypeImage *>(OpaqueTy);
        auto Desc = ImageTy->getDescriptor();
        assert(Desc.Sampled <= 2); // 0 - runtime, 1 - sampled, 2 - non sampled

        if (Desc.Sampled == 2) {
          // For a storage image, build the metadata
          ShaderImageMemoryMetadata ImageMemoryMD = {};
          if (BV->hasDecorate(DecorationRestrict))
            ImageMemoryMD.Restrict = true;
          if (BV->hasDecorate(DecorationCoherent))
            ImageMemoryMD.Coherent = true;
          if (BV->hasDecorate(DecorationVolatile))
            ImageMemoryMD.Volatile = true;
          if (BV->hasDecorate(DecorationNonWritable))
            ImageMemoryMD.NonWritable = true;
          if (BV->hasDecorate(DecorationNonReadable))
            ImageMemoryMD.NonReadable = true;

          std::vector<Metadata*> ImageMemoryMDs;
          ImageMemoryMDs.push_back(ConstantAsMetadata::get(
            ConstantInt::get(Int32Ty, ImageMemoryMD.U32All)));
          auto ImageMemoryMDNode = MDNode::get(*Context, ImageMemoryMDs);
          GV->addMetadata(gSPIRVMD::ImageMemory, *ImageMemoryMDNode);
        }
      }
    }
  } else if(BV->hasType() &&
      // Handle those instructions relevant to variable pointer storage buffer
      BV->getType()->isTypePointer() &&
      BV->getType()->getPointerStorageClass()
        == StorageClassStorageBuffer &&
      EnableVarPtr) {

    auto Inst = dyn_cast<Instruction>(V);
    // NOTE: storage pointer could be null pointer in some tests,
    // so instruction should be ignored in this case
    if (Inst != nullptr) {
      auto BTy = transType(BV->getType());

      // Build block metadata
      ShaderBlockDecorate BlockDec = {};
      Type *BlockMDTy = nullptr;
      auto BlockMD = buildShaderBlockMetadata(BV->getType(), BlockDec, BlockMDTy);

      std::vector<Metadata*> BlockMDs;
      BlockMDs.push_back(ConstantAsMetadata::get(BlockMD));
      auto BlockMDNode = MDNode::get(*Context, BlockMDs);
      Inst->setMetadata(gSPIRVMD::Block, BlockMDNode);
    }
  } else {
    bool IsNonUniform = BV->hasDecorate(DecorationNonUniformEXT);
    if (IsNonUniform && isa<Instruction>(V)) {
      std::string  MangledFuncName = "";
      std::vector<Value*> Args;
      Args.push_back(V);
      auto Types = getTypes(Args);
      auto VoidTy = Type::getVoidTy(*Context);
      auto BB = cast<Instruction>(V)->getParent();

      // Per-instruction metadata is not safe, LLVM optimizer may remove them,
      // so we choose to add a dummy instruction and remove them when it isn't
      // needed.
      mangleGlslBuiltin(gSPIRVMD::NonUniform, Types, MangledFuncName);
      auto F = getOrCreateFunction(M, VoidTy, Types, MangledFuncName);
      auto CI = CallInst::Create(F, Args, "", BB);
    }
  }

  return true;
}

// Calculates shader block size
uint32_t SPIRVToLLVM::calcShaderBlockSize(SPIRVType *BT, uint32_t BlockSize,
                                          uint32_t MatrixStride,
                                          bool IsRowMajor) {
  if (BT->isTypeStruct()) {
    if (BT->getStructMemberCount() == 0)
      BlockSize = 0;
    else {
      // Find member with max offset
      uint32_t MemberIdxWithMaxOffset = 0;
      uint32_t MaxOffset = 0;
      for (uint32_t MemberIdx = 0; MemberIdx < BT->getStructMemberCount();
          ++MemberIdx) {
        uint32_t Offset = 0;
        if (BT->hasMemberDecorate(MemberIdx, DecorationOffset, 0, &Offset)) {
          if (Offset > MaxOffset) {
            MaxOffset = Offset;
            MemberIdxWithMaxOffset = MemberIdx;
          }
        } else
          llvm_unreachable("Missing offset decoration");
      }

      uint32_t MemberMatrixStride = MatrixStride;
      BT->hasMemberDecorate(MemberIdxWithMaxOffset, DecorationMatrixStride, 0,
          &MemberMatrixStride);

      bool IsMemberRowMajor = IsRowMajor;
      if (BT->hasMemberDecorate(MemberIdxWithMaxOffset, DecorationRowMajor))
        IsMemberRowMajor = true;
      else if (BT->hasMemberDecorate(MemberIdxWithMaxOffset, DecorationColMajor))
        IsMemberRowMajor = false;

      SPIRVType *MemberTy = BT->getStructMemberType(MemberIdxWithMaxOffset);
      BlockSize += calcShaderBlockSize(MemberTy, MaxOffset, MemberMatrixStride,
          IsMemberRowMajor);
    }
  } else if (BT->isTypeArray() || BT->isTypeMatrix()) {
    if (BT->isTypeArray()) {
      uint32_t ArrayStride = 0;
      if (!BT->hasDecorate(DecorationArrayStride, 0, &ArrayStride))
        llvm_unreachable("Missing array stride decoration");
      uint32_t NumElems = BT->getArrayLength();
      BlockSize += NumElems * ArrayStride;
    } else {
      assert(MatrixStride != SPIRVID_INVALID);
      uint32_t NumVectors =
        IsRowMajor ? BT->getMatrixColumnType()->getVectorComponentCount() :
          BT->getMatrixColumnCount();
      BlockSize += NumVectors * MatrixStride;
    }
  } else if (BT->isTypeVector()) {
    uint32_t SizeInBytes = BT->getVectorComponentType()->getBitWidth() / 8;
    uint32_t NumComps = BT->getVectorComponentCount();
    BlockSize += SizeInBytes * NumComps;
  } else if (BT->isTypeScalar()) {
    uint32_t SizeInBytes = BT->getBitWidth() / 8;
    BlockSize += SizeInBytes;
  }
  else
      llvm_unreachable("Invalid shader block type");

  return BlockSize;
}

// Builds shader input/output metadata.
Constant * SPIRVToLLVM::buildShaderInOutMetadata(SPIRVType *BT,
                                                 ShaderInOutDecorate &InOutDec,
                                                 Type *&MDTy) {
  SPIRVWord Loc = SPIRVID_INVALID;
  if (BT->hasDecorate(DecorationLocation, 0, &Loc)) {
    InOutDec.Value.Loc = Loc;
    InOutDec.IsBuiltIn = false;
  }

  SPIRVWord Index = SPIRVID_INVALID;
  if (BT->hasDecorate(DecorationIndex, 0, &Index)) {
      InOutDec.Index = Index;
  }

  SPIRVWord BuiltIn = SPIRVID_INVALID;
  if (BT->hasDecorate(DecorationBuiltIn, 0, &BuiltIn)) {
    InOutDec.Value.BuiltIn = BuiltIn;
    InOutDec.IsBuiltIn = true;
  }

  SPIRVWord Component = SPIRVID_INVALID;
  if (BT->hasDecorate(DecorationComponent, 0, &Component))
    InOutDec.Component = Component;

  if (BT->hasDecorate(DecorationFlat))
    InOutDec.Interp.Mode = InterpModeFlat;

  if (BT->hasDecorate(DecorationNoPerspective))
    InOutDec.Interp.Mode = InterpModeNoPersp;

  if (BT->hasDecorate(DecorationCentroid))
    InOutDec.Interp.Loc = InterpLocCentroid;

  if (BT->hasDecorate(DecorationSample))
    InOutDec.Interp.Loc = InterpLocSample;

  if (BT->hasDecorate(DecorationExplicitInterpAMD)) {
    InOutDec.Interp.Mode = InterpModeCustom;
    InOutDec.Interp.Loc = InterpLocCustom;
  }

  if (BT->hasDecorate(DecorationPatch))
    InOutDec.PerPatch = true;

  SPIRVWord StreamId = SPIRVID_INVALID;
  if (BT->hasDecorate(DecorationStream, 0, &StreamId))
    InOutDec.StreamId = StreamId;

  if (BT->isTypeScalar() || BT->isTypeVector()) {
    // Hanlde scalar or vector type
    assert(InOutDec.Value.U32All != SPIRVID_INVALID);

    // Build metadata for the scala/vector
    ShaderInOutMetadata InOutMD = {};
    if (InOutDec.IsBuiltIn) {
      InOutMD.IsBuiltIn = true;
      InOutMD.IsLoc = false;
      InOutMD.Value = InOutDec.Value.BuiltIn;
    } else {
      InOutMD.IsLoc = true;
      InOutMD.IsBuiltIn = false;
      InOutMD.Value = InOutDec.Value.Loc;
      InOutMD.Index = InOutDec.Index;
    }

    InOutMD.Component = InOutDec.Component;
    InOutMD.InterpMode = InOutDec.Interp.Mode;
    InOutMD.InterpLoc = InOutDec.Interp.Loc;
    InOutMD.PerPatch = InOutDec.PerPatch;
    InOutMD.StreamId = InOutDec.StreamId;

    // Check signedness for generic input/output
    if (!InOutDec.IsBuiltIn) {
      SPIRVType *ScalarTy =
        BT->isTypeVector() ? BT->getVectorComponentType() : BT;
      if (ScalarTy->isTypeInt())
        InOutMD.Signedness = static_cast<SPIRVTypeInt*>(ScalarTy)->isSigned();
    }

    // Update next location value
    if (!InOutDec.IsBuiltIn) {
      auto Width = BT->getBitWidth();
      if (BT->isTypeVector())
        Width *= BT->getVectorComponentCount();
      assert(Width <= 64 * 4);

      InOutDec.Value.Loc += (Width <= 32 * 4) ? 1 : 2;
    }

    MDTy = Type::getInt32Ty(*Context);
    return ConstantInt::get(MDTy, InOutMD.U32All);

  } else if (BT->isTypeArray() || BT->isTypeMatrix()) {
    // Handle array or matrix type
    auto Int32Ty = Type::getInt32Ty(*Context);

    // Build element metadata
    auto ElemTy = BT->isTypeArray() ? BT->getArrayElementType() :
                                      BT->getMatrixColumnType();
    uint32_t StartLoc = InOutDec.Value.Loc;
    Type *ElemMDTy = nullptr;
    auto ElemDec = InOutDec; // Inherit from parent
    auto ElemMD = buildShaderInOutMetadata(ElemTy, ElemDec, ElemMDTy);

    if (ElemDec.PerPatch)
      InOutDec.PerPatch = true; // Set "per-patch" flag

    uint32_t Stride = ElemDec.Value.Loc - StartLoc;
    uint32_t NumElems = BT->isTypeArray() ? BT->getArrayLength() :
                                            BT->getMatrixColumnCount();

    // Update next location value
    if (!InOutDec.IsBuiltIn)
      InOutDec.Value.Loc = StartLoc + (Stride * NumElems);

    // Built metadata for the array/matrix
    std::vector<Type *> MDTys;
    MDTys.push_back(Int32Ty);     // Stride
    MDTys.push_back(Int32Ty);     // Content of "ShaderInOutMetadata"
    MDTys.push_back(ElemMDTy);    // Element MD type
    MDTy = StructType::get(*Context, MDTys);

    ShaderInOutMetadata InOutMD = {};
    if (InOutDec.IsBuiltIn) {
      InOutMD.IsBuiltIn = true;
      InOutMD.IsLoc = false;
      InOutMD.Value = InOutDec.Value.BuiltIn;
    } else {
      InOutMD.IsLoc = true;
      InOutMD.IsBuiltIn = false;
      InOutMD.Value = StartLoc;
    }

    InOutMD.Component = InOutDec.Component;
    InOutMD.InterpMode = InOutDec.Interp.Mode;
    InOutMD.InterpLoc = InOutDec.Interp.Loc;
    InOutMD.PerPatch = InOutDec.PerPatch;
    InOutMD.StreamId = InOutDec.StreamId;

    std::vector<Constant *> MDValues;
    MDValues.push_back(ConstantInt::get(Int32Ty, Stride));
    MDValues.push_back(ConstantInt::get(Int32Ty, InOutMD.U32All));
    MDValues.push_back(ElemMD);
    return ConstantStruct::get(static_cast<StructType *>(MDTy), MDValues);

  } else if (BT->isTypeStruct()) {
    // Handle structure type
    std::vector<Type *>     MemberMDTys;
    std::vector<Constant *> MemberMDValues;

    // Build metadata for each structure member
    auto NumMembers = BT->getStructMemberCount();
    for (auto MemberIdx = 0; MemberIdx < NumMembers; ++MemberIdx) {
      auto MemberDec = InOutDec;

      SPIRVWord MemberLoc = SPIRVID_INVALID;
      if (BT->hasMemberDecorate(
            MemberIdx, DecorationLocation, 0, &MemberLoc)) {
        MemberDec.IsBuiltIn = false;
        MemberDec.Value.Loc = MemberLoc;
      }

      SPIRVWord MemberBuiltIn = SPIRVID_INVALID;
      if (BT->hasMemberDecorate(
            MemberIdx, DecorationBuiltIn, 0, &MemberBuiltIn)) {
        MemberDec.IsBuiltIn = true;
        MemberDec.Value.BuiltIn = MemberBuiltIn;
      }

      SPIRVWord MemberComponent = SPIRVID_INVALID;
      if (BT->hasMemberDecorate(
            MemberIdx, DecorationComponent, 0, &MemberComponent))
        MemberDec.Component = Component;

      if (BT->hasMemberDecorate(MemberIdx, DecorationFlat))
        MemberDec.Interp.Mode = InterpModeFlat;

      if (BT->hasMemberDecorate(MemberIdx, DecorationNoPerspective))
        MemberDec.Interp.Mode = InterpModeNoPersp;

      if (BT->hasMemberDecorate(MemberIdx, DecorationCentroid))
        MemberDec.Interp.Loc = InterpLocCentroid;

      if (BT->hasMemberDecorate(MemberIdx, DecorationSample))
        MemberDec.Interp.Loc = InterpLocSample;

      if (BT->hasMemberDecorate(MemberIdx, DecorationExplicitInterpAMD)) {
        MemberDec.Interp.Mode = InterpModeCustom;
        MemberDec.Interp.Loc = InterpLocCustom;
      }

      if (BT->hasMemberDecorate(MemberIdx, DecorationPatch))
        MemberDec.PerPatch = true;

      SPIRVWord MemberStreamId = SPIRVID_INVALID;
      if (BT->hasMemberDecorate(
            MemberIdx, DecorationStream, 0, &MemberStreamId))
        MemberDec.StreamId = MemberStreamId;

      auto  MemberTy = BT->getStructMemberType(MemberIdx);
      Type *MemberMDTy = nullptr;
      auto  MemberMD   =
        buildShaderInOutMetadata(MemberTy, MemberDec, MemberMDTy);

      if (MemberDec.IsBuiltIn)
        InOutDec.IsBuiltIn = true; // Set "builtin" flag
      else
        InOutDec.Value.Loc = MemberDec.Value.Loc; // Update next location value

      if (MemberDec.PerPatch)
        InOutDec.PerPatch = true; // Set "per-patch" flag

      MemberMDTys.push_back(MemberMDTy);
      MemberMDValues.push_back(MemberMD);
    }

    // Build  metadata for the structure
    MDTy = StructType::get(*Context, MemberMDTys);
    return ConstantStruct::get(static_cast<StructType *>(MDTy), MemberMDValues);
  }

  llvm_unreachable("Invalid type");
  return nullptr;
}

// Builds shader block metadata.
Constant * SPIRVToLLVM::buildShaderBlockMetadata(SPIRVType *BT,
                                                 ShaderBlockDecorate &BlockDec,
                                                 Type *&MDTy) {
  const bool IsUniformBlock = BT->hasDecorate(DecorationBlock);
  if (BT->isTypeVector() || BT->isTypeScalar()) {
    // Handle scalar or vector type
    ShaderBlockMetadata BlockMD = {};
    BlockMD.offset       = BlockDec.Offset;
    BlockMD.IsMatrix     = false; // Scalar or vector, clear matrix flag
    BlockMD.IsRowMajor   = BlockDec.IsRowMajor;
    BlockMD.MatrixStride = BlockDec.MatrixStride;
    BlockMD.Restrict     = BlockDec.Restrict;
    BlockMD.Coherent     = BlockDec.Coherent;
    BlockMD.Volatile     = BlockDec.Volatile;
    BlockMD.NonWritable  = BlockDec.NonWritable || IsUniformBlock;
    BlockMD.NonReadable  = BlockDec.NonReadable;

    MDTy = Type::getInt64Ty(*Context);
    return ConstantInt::get(MDTy, BlockMD.U64All);

  } else if (BT->isTypeArray() || BT->isTypeMatrix() || BT->isTypePointer()) {
    // Handle array or matrix type
    auto Int32Ty = Type::getInt32Ty(*Context);
    auto Int64Ty = Type::getInt64Ty(*Context);

    uint32_t  Stride = 0;
    SPIRVType *ElemTy = nullptr;
    ShaderBlockMetadata BlockMD = {};
    if (BT->isTypeArray()) {
      // NOTE: Here, we should keep matrix stride and the flag of row-major
      // matrix. For SPIR-V, such decorations are specified on structure
      // members.
      BlockDec.IsMatrix = false;
      SPIRVWord ArrayStride = 0;
      if (!BT->hasDecorate(DecorationArrayStride, 0, &ArrayStride))
        llvm_unreachable("Missing array stride decoration");
      Stride = ArrayStride;
      ElemTy = BT->getArrayElementType();

    } else if (BT->isTypePointer()) {
      BlockDec.IsMatrix = false;
      SPIRVWord ArrayStride = 0;
      BT->hasDecorate(DecorationArrayStride, 0, &ArrayStride);
      Stride = ArrayStride;
      ElemTy = BT->getPointerElementType();
      BlockMD.IsPointer = true;
    } else {
      BlockDec.IsMatrix = true;
      Stride = BlockDec.MatrixStride;
      ElemTy = BT->getMatrixColumnType();
    }

    // Build element metadata
    Type *ElemMDTy = nullptr;
    auto ElemDec = BlockDec; // Inherit from parent
    ElemDec.Offset = 0; // Offset should be cleared for the element type of array, pointer, matrix
    auto ElemMD = buildShaderBlockMetadata(ElemTy, ElemDec, ElemMDTy);

    // Build metadata for the array/matrix
    std::vector<Type *> MDTys;
    MDTys.push_back(Int32Ty);   // Stride
    MDTys.push_back(Int64Ty);   // Content of ShaderBlockMetadata
    MDTys.push_back(ElemMDTy);  // Element MD type
    MDTy = StructType::get(*Context, MDTys);

    BlockMD.offset        = BlockDec.Offset;
    BlockMD.IsMatrix      = BlockDec.IsMatrix;
    BlockMD.IsRowMajor    = BlockDec.IsRowMajor;
    BlockMD.MatrixStride  = BlockDec.MatrixStride;
    BlockMD.Restrict      = BlockDec.Restrict;
    BlockMD.Coherent      = BlockDec.Coherent;
    BlockMD.Volatile      = BlockDec.Volatile;
    BlockMD.NonWritable   = BlockDec.NonWritable || IsUniformBlock;
    BlockMD.NonReadable   = BlockDec.NonReadable;

    std::vector<Constant *> MDValues;
    MDValues.push_back(ConstantInt::get(Int32Ty, Stride));
    MDValues.push_back(ConstantInt::get(Int64Ty, BlockMD.U64All));
    MDValues.push_back(ElemMD);
    return ConstantStruct::get(static_cast<StructType *>(MDTy), MDValues);

  } else if (BT->isTypeStruct()) {
    // Handle structure type
    BlockDec.IsMatrix = false;

    std::vector<Type *>     MemberMDTys;
    std::vector<Constant *> MemberMDValues;

    // Build metadata for each structure member
    uint32_t NumMembers = BT->getStructMemberCount();
    for (uint32_t MemberIdx = 0; MemberIdx < NumMembers; ++MemberIdx) {
      SPIRVWord MemberOffset       = 0;
      SPIRVWord MemberArrayStride  = 0;
      SPIRVWord MemberMatrixStride = 0;

      // Check member decorations
      auto MemberDec = BlockDec; // Inherit from parent
      if (BT->hasMemberDecorate(MemberIdx, DecorationOffset, 0, &MemberOffset))
        MemberDec.Offset = MemberOffset;
      else
        llvm_unreachable("Missing offset decoration");

      if (BT->hasMemberDecorate(MemberIdx, DecorationMatrixStride,
                                0, &MemberMatrixStride))
        MemberDec.MatrixStride = MemberMatrixStride;

      if (BT->hasMemberDecorate(MemberIdx, DecorationRowMajor))
        MemberDec.IsRowMajor = true;
      else if (BT->hasMemberDecorate(MemberIdx, DecorationColMajor))
        MemberDec.IsRowMajor = false;

      if (BT->hasMemberDecorate(MemberIdx, DecorationRestrict))
        MemberDec.Restrict = true;
      if (BT->hasMemberDecorate(MemberIdx, DecorationCoherent))
        MemberDec.Coherent = true;
      if (BT->hasMemberDecorate(MemberIdx, DecorationVolatile))
        MemberDec.Volatile = true;
      if (BT->hasMemberDecorate(MemberIdx, DecorationNonWritable))
        MemberDec.NonWritable = true;
      if (BT->hasMemberDecorate(MemberIdx, DecorationNonReadable))
        MemberDec.NonReadable = true;
      MemberDec.NonWritable = MemberDec.NonWritable || IsUniformBlock;

      // Build metadata for structure member
      auto MemberTy = BT->getStructMemberType(MemberIdx);
      Type *MemberMDTy = nullptr;
      auto MemberMeta = buildShaderBlockMetadata(MemberTy, MemberDec, MemberMDTy);
      MemberMDTys.push_back(MemberMDTy);
      MemberMDValues.push_back(MemberMeta);
    }

    // Build metadata for the structure
    // Member structure type and value
    auto StructMDTy = StructType::get(*Context, MemberMDTys);
    auto StructMD = ConstantStruct::get(static_cast<StructType *>(StructMDTy), MemberMDValues);
    auto Int64Ty = Type::getInt64Ty(*Context);
    ShaderBlockMetadata BlockMD = {};
    BlockMD.offset = BlockDec.Offset;
    BlockMD.IsStruct = true;

    // Construct structure metadata
    std::vector<Type *> MDTys;
    MDTys.push_back(Int64Ty);     // Content of ShaderBlockMetadata
    MDTys.push_back(StructMDTy);  // Structure MD type

    // Structure MD type
    MDTy = StructType::get(*Context, MDTys);
    std::vector<Constant *> MDValues;
    MDValues.push_back(ConstantInt::get(Int64Ty, BlockMD.U64All));
    MDValues.push_back(StructMD);

    return ConstantStruct::get(static_cast<StructType *>(MDTy), MDValues);
  }

  llvm_unreachable("Invalid type");
  return nullptr;
}

void SPIRVToLLVM::transOCLVectorLoadStore(std::string &UnmangledName,
                                          std::vector<SPIRVWord> &BArgs) {
  if (UnmangledName.find("vload") == 0 &&
      UnmangledName.find("n") != std::string::npos) {
    if (BArgs.back() != 1) {
      std::stringstream SS;
      SS << BArgs.back();
      UnmangledName.replace(UnmangledName.find("n"), 1, SS.str());
    } else {
      UnmangledName.erase(UnmangledName.find("n"), 1);
    }
    BArgs.pop_back();
  } else if (UnmangledName.find("vstore") == 0) {
    if (UnmangledName.find("n") != std::string::npos) {
      auto T = BM->getValueType(BArgs[0]);
      if (T->isTypeVector()) {
        auto W = T->getVectorComponentCount();
        std::stringstream SS;
        SS << W;
        UnmangledName.replace(UnmangledName.find("n"), 1, SS.str());
      } else {
        UnmangledName.erase(UnmangledName.find("n"), 1);
      }
    }
    if (UnmangledName.find("_r") != std::string::npos) {
      UnmangledName.replace(
          UnmangledName.find("_r"), 2,
          std::string("_") +
              SPIRSPIRVFPRoundingModeMap::rmap(
                  static_cast<SPIRVFPRoundingModeKind>(BArgs.back())));
      BArgs.pop_back();
    }
   }
}

// printf is not mangled. The function type should have just one argument.
// read_image*: the second argument should be mangled as sampler.
Instruction *SPIRVToLLVM::transOCLBuiltinFromExtInst(SPIRVExtInst *BC,
                                                     BasicBlock *BB) {
  assert(BB && "Invalid BB");
  std::string MangledName;
  SPIRVWord EntryPoint = BC->getExtOp();
  bool IsVarArg = false;
  bool IsPrintf = false;
  std::string UnmangledName;
  auto BArgs = BC->getArguments();

  assert(BM->getBuiltinSet(BC->getExtSetId()) == SPIRVEIS_OpenCL &&
         "Not OpenCL extended instruction");
  if (EntryPoint == OpenCLLIB::Printf)
    IsPrintf = true;
  else {
    UnmangledName = OCLExtOpMap::map(static_cast<OCLExtOpKind>(EntryPoint));
  }

  SPIRVDBG(spvdbgs() << "[transOCLBuiltinFromExtInst] OrigUnmangledName: "
                     << UnmangledName << '\n');
  transOCLVectorLoadStore(UnmangledName, BArgs);

  std::vector<Type *> ArgTypes = transTypeVector(BC->getValueTypes(BArgs));

  if (IsPrintf) {
    MangledName = "printf";
    IsVarArg = true;
    ArgTypes.resize(1);
  } else if (UnmangledName.find("read_image") == 0) {
    auto ModifiedArgTypes = ArgTypes;
    ModifiedArgTypes[1] = getOrCreateOpaquePtrType(M, "opencl.sampler_t");
    mangleOpenClBuiltin(UnmangledName, ModifiedArgTypes, MangledName);
  } else {
    mangleOpenClBuiltin(UnmangledName, ArgTypes, MangledName);
  }
  SPIRVDBG(spvdbgs() << "[transOCLBuiltinFromExtInst] ModifiedUnmangledName: "
                     << UnmangledName << " MangledName: " << MangledName
                     << '\n');

  FunctionType *FT =
      FunctionType::get(transType(BC->getType()), ArgTypes, IsVarArg);
  Function *F = M->getFunction(MangledName);
  if (!F) {
    F = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    F->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
  }
  auto Args = transValue(BC->getValues(BArgs), F, BB);
  SPIRVDBG(dbgs() << "[transOCLBuiltinFromExtInst] Function: " << *F
                  << ", Args: ";
           for (auto &I
                : Args) dbgs()
           << *I << ", ";
           dbgs() << '\n');
  CallInst *Call = CallInst::Create(F, Args, BC->getName(), BB);
  setCallingConv(Call);
  addFnAttr(Context, Call, Attribute::NoUnwind);
  return transOCLBuiltinPostproc(BC, Call, BB, UnmangledName);
}

Instruction * SPIRVToLLVM::transGLSLBuiltinFromExtInst(SPIRVExtInst *BC,
                                                       BasicBlock *BB) {
  assert(BB && "Invalid BB");

  SPIRVExtInstSetKind Set = BM->getBuiltinSet(BC->getExtSetId());
  assert((Set == SPIRVEIS_GLSL ||
          Set == SPIRVEIS_ShaderBallotAMD ||
          Set == SPIRVEIS_ShaderExplicitVertexParameterAMD ||
          Set == SPIRVEIS_GcnShaderAMD ||
          Set == SPIRVEIS_ShaderTrinaryMinMaxAMD)
         && "Not valid extended instruction");

  SPIRVWord EntryPoint = BC->getExtOp();
  auto BArgs = BC->getArguments();
  std::vector<Type *> ArgTys = transTypeVector(BC->getValueTypes(BArgs));
  string UnmangledName = "";
  if (Set == SPIRVEIS_GLSL)
    UnmangledName =  GLSLExtOpMap::map(
      static_cast<GLSLExtOpKind>(EntryPoint));
  else if (Set == SPIRVEIS_ShaderBallotAMD)
    UnmangledName = ShaderBallotAMDExtOpMap::map(
      static_cast<ShaderBallotAMDExtOpKind>(EntryPoint));
  else if (Set == SPIRVEIS_ShaderExplicitVertexParameterAMD)
    UnmangledName = ShaderExplicitVertexParameterAMDExtOpMap::map(
      static_cast<ShaderExplicitVertexParameterAMDExtOpKind>(EntryPoint));
  else if (Set == SPIRVEIS_GcnShaderAMD)
    UnmangledName = GcnShaderAMDExtOpMap::map(
      static_cast<GcnShaderAMDExtOpKind>(EntryPoint));
  else if (Set == SPIRVEIS_ShaderTrinaryMinMaxAMD)
    UnmangledName = ShaderTrinaryMinMaxAMDExtOpMap::map(
      static_cast<ShaderTrinaryMinMaxAMDExtOpKind>(EntryPoint));

  string MangledName;
  mangleGlslBuiltin(UnmangledName, ArgTys, MangledName);
  if (static_cast<GLSLExtOpKind>(EntryPoint) == GLSLstd450FrexpStruct) {
    // NOTE: For frexp(), the input floating-point value is float16, we have
    // two overloading versions:
    //     f16vec frexp(f16vec, ivec)
    //     f16vec frexp(f16vec, i16vec)
    //
    // However, glslang translates "frexp" to "FrexpStruct". We have to check
    // the result type to revise the mangled name to differentiate such two
    // variants.
    assert(BC->getType()->isTypeStruct());
    auto MantTy = BC->getType()->getStructMemberType(0);
    auto ExpTy = BC->getType()->getStructMemberType(1);
    if (MantTy->isTypeVectorOrScalarFloat(16)) {
      if (ExpTy->isTypeVector()) {
        auto CompCount = ExpTy->getVectorComponentCount();
        MangledName += "Dv" + std::to_string(CompCount) + "_";
      }

      MangledName += ExpTy->isTypeVectorOrScalarInt(16) ? "s" : "i";
    }
  }

  FunctionType *FuncTy = FunctionType::get(transType(BC->getType()),
                                           ArgTys,
                                           false);
  Function *Func = M->getFunction(MangledName);
  if (!Func) {
    Func = Function::Create(FuncTy,
                            GlobalValue::ExternalLinkage,
                            MangledName,
                            M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);
  }
  auto Args = transValue(BC->getValues(BArgs), Func, BB);
  SPIRVDBG(dbgs() << "[transGLSLBuiltinFromExtInst] Function: " << *Func <<
    ", Args: ";
  for (auto &iter : Args) dbgs() << *iter << ", "; dbgs() << '\n');
  CallInst *Call = CallInst::Create(Func,
                                    Args,
                                    BC->getName(),
                                    BB);
  setCallingConv(Call);
  addFnAttr(Context, Call, Attribute::NoUnwind);
  return Call;
}

CallInst *SPIRVToLLVM::transOCLBarrier(BasicBlock *BB, SPIRVWord ExecScope,
                                       SPIRVWord MemSema, SPIRVWord MemScope) {
  SPIRVWord Ver = 0;
  BM->getSourceLanguage(&Ver);

  Type *Int32Ty = Type::getInt32Ty(*Context);
  Type *VoidTy = Type::getVoidTy(*Context);

  std::string FuncName;
  SmallVector<Type *, 2> ArgTy;
  SmallVector<Value *, 2> Arg;

  Constant *MemFenceFlags =
      ConstantInt::get(Int32Ty, rmapBitMask<OCLMemFenceMap>(MemSema));

  FuncName = (ExecScope == ScopeWorkgroup) ? kOCLBuiltinName::WorkGroupBarrier
                                           : kOCLBuiltinName::SubGroupBarrier;

  if (ExecScope == ScopeWorkgroup && Ver > 0 && Ver <= kOCLVer::CL12) {
    FuncName = kOCLBuiltinName::Barrier;
    ArgTy.push_back(Int32Ty);
    Arg.push_back(MemFenceFlags);
  } else {
    Constant *Scope = ConstantInt::get(
        Int32Ty, OCLMemScopeMap::rmap(static_cast<spv::Scope>(MemScope)));

    ArgTy.append(2, Int32Ty);
    Arg.push_back(MemFenceFlags);
    Arg.push_back(Scope);
  }

  std::string MangledName;

  mangleOpenClBuiltin(FuncName, ArgTy, MangledName);
  Function *Func = M->getFunction(MangledName);
  if (!Func) {
    FunctionType *FT = FunctionType::get(VoidTy, ArgTy, false);
    Func = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);
  }

  return CallInst::Create(Func, Arg, "", BB);
}

CallInst *SPIRVToLLVM::transOCLMemFence(BasicBlock *BB, SPIRVWord MemSema,
                                        SPIRVWord MemScope) {
  SPIRVWord Ver = 0;
  BM->getSourceLanguage(&Ver);

  Type *Int32Ty = Type::getInt32Ty(*Context);
  Type *VoidTy = Type::getVoidTy(*Context);

  std::string FuncName;
  SmallVector<Type *, 3> ArgTy;
  SmallVector<Value *, 3> Arg;

  Constant *MemFenceFlags =
      ConstantInt::get(Int32Ty, rmapBitMask<OCLMemFenceMap>(MemSema));

  if (Ver > 0 && Ver <= kOCLVer::CL12) {
    FuncName = kOCLBuiltinName::MemFence;
    ArgTy.push_back(Int32Ty);
    Arg.push_back(MemFenceFlags);
  } else {
    Constant *Order = ConstantInt::get(Int32Ty, mapSPIRVMemOrderToOCL(MemSema));

    Constant *Scope = ConstantInt::get(
        Int32Ty, OCLMemScopeMap::rmap(static_cast<spv::Scope>(MemScope)));

    FuncName = kOCLBuiltinName::AtomicWorkItemFence;
    ArgTy.append(3, Int32Ty);
    Arg.push_back(MemFenceFlags);
    Arg.push_back(Order);
    Arg.push_back(Scope);
  }

  std::string MangledName;

  mangleOpenClBuiltin(FuncName, ArgTy, MangledName);
  Function *Func = M->getFunction(MangledName);
  if (!Func) {
    FunctionType *FT = FunctionType::get(VoidTy, ArgTy, false);
    Func = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);
  }

  return CallInst::Create(Func, Arg, "", BB);
}

Instruction *SPIRVToLLVM::transOCLBarrierFence(SPIRVInstruction *MB,
                                               BasicBlock *BB) {
  assert(BB && "Invalid BB");
  std::string FuncName;
  auto GetIntVal = [](SPIRVValue *Value) {
    return static_cast<SPIRVConstant *>(Value)->getZExtIntValue();
  };

  CallInst *Call = nullptr;

  if (MB->getOpCode() == OpMemoryBarrier) {
    auto MemB = static_cast<SPIRVMemoryBarrier *>(MB);

    SPIRVWord MemScope = GetIntVal(MemB->getOpValue(0));
    SPIRVWord MemSema = GetIntVal(MemB->getOpValue(1));

    Call = transOCLMemFence(BB, MemSema, MemScope);
  } else if (MB->getOpCode() == OpControlBarrier) {
    auto CtlB = static_cast<SPIRVControlBarrier *>(MB);

    SPIRVWord ExecScope = GetIntVal(CtlB->getExecScope());
    SPIRVWord MemSema = GetIntVal(CtlB->getMemSemantic());
    SPIRVWord MemScope = GetIntVal(CtlB->getMemScope());

    Call = transOCLBarrier(BB, ExecScope, MemSema, MemScope);
  } else {
    llvm_unreachable("Invalid instruction");
  }

  setName(Call, MB);
  setAttrByCalledFunc(Call);
  SPIRVDBG(spvdbgs() << "[transBarrier] " << *MB << " -> ";
           dbgs() << *Call << '\n';)

  return Call;
}

// SPIR-V only contains language version. Use OpenCL language version as
// SPIR version.
bool SPIRVToLLVM::transSourceLanguage() {
  SPIRVWord Ver = 0;
  SourceLanguage Lang = BM->getSourceLanguage(&Ver);
  assert((Lang == SourceLanguageUnknown ||
          Lang == SourceLanguageOpenCL_C ||
          Lang == SourceLanguageOpenCL_CPP ||
          Lang == SourceLanguageGLSL ||
          Lang == SourceLanguageESSL ||
          Lang == SourceLanguageHLSL) && "Unsupported source language");
  unsigned short Major = 0;
  unsigned char Minor = 0;
  unsigned char Rev = 0;
  if (Lang == SourceLanguageOpenCL_C || Lang == SourceLanguageOpenCL_CPP)
    std::tie(Major, Minor, Rev) = decodeOCLVer(Ver);
  else if (Lang == SourceLanguageGLSL ||
           Lang == SourceLanguageESSL ||
           Lang == SourceLanguageHLSL)
    std::tie(Major, Minor, Rev) = decodeGLVer(Ver);
  SPIRVMDBuilder Builder(*M);
  Builder.addNamedMD(kSPIRVMD::Source).addOp().add(Lang).add(Ver).done();
  if (Lang == SourceLanguageOpenCL_C || Lang == SourceLanguageOpenCL_CPP) {
    // ToDo: Phasing out usage of old SPIR metadata
    if (Ver <= kOCLVer::CL12)
      addOCLVersionMetadata(Context, M, kSPIR2MD::SPIRVer, 1, 2);
    else
      addOCLVersionMetadata(Context, M, kSPIR2MD::SPIRVer, 2, 0);

    addOCLVersionMetadata(Context, M, kSPIR2MD::OCLVer, Major, Minor);
  } else if (Lang == SourceLanguageGLSL ||
             Lang == SourceLanguageESSL ||
             Lang == SourceLanguageHLSL) {
    // TODO: Add GL version metadata.
  }
  return true;
}

bool SPIRVToLLVM::transSourceExtension() {
  auto ExtSet = rmap<OclExt::Kind>(BM->getExtension());
  auto CapSet = rmap<OclExt::Kind>(BM->getCapability());
  ExtSet.insert(CapSet.begin(), CapSet.end());
  auto OCLExtensions = map<std::string>(ExtSet);
  std::set<std::string> OCLOptionalCoreFeatures;
  static const char *OCLOptCoreFeatureNames[] = {
      "cl_images",
      "cl_doubles",
  };
  for (auto &I : OCLOptCoreFeatureNames) {
    auto Loc = OCLExtensions.find(I);
    if (Loc != OCLExtensions.end()) {
      OCLExtensions.erase(Loc);
      OCLOptionalCoreFeatures.insert(I);
    }
  }
  addNamedMetadataStringSet(Context, M, kSPIR2MD::Extensions, OCLExtensions);
  addNamedMetadataStringSet(Context, M, kSPIR2MD::OptFeatures,
                            OCLOptionalCoreFeatures);
  return true;
}

// If the argument is unsigned return uconvert*, otherwise return convert*.
std::string SPIRVToLLVM::getOCLConvertBuiltinName(SPIRVInstruction *BI) {
  auto OC = BI->getOpCode();
  assert(isCvtOpCode(OC) && "Not convert instruction");
  auto U = static_cast<SPIRVUnary *>(BI);
  std::string Name;
  if (isCvtFromUnsignedOpCode(OC))
    Name = "u";
  Name += "convert_";
  Name += mapSPIRVTypeToOCLType(U->getType(), !isCvtToUnsignedOpCode(OC));
  SPIRVFPRoundingModeKind Rounding;
  if (U->isSaturatedConversion())
    Name += "_sat";
  if (U->hasFPRoundingMode(&Rounding)) {
    Name += "_";
    Name += SPIRSPIRVFPRoundingModeMap::rmap(Rounding);
  }
  return Name;
}

// Check Address Space of the Pointer Type
std::string SPIRVToLLVM::getOCLGenericCastToPtrName(SPIRVInstruction *BI) {
  auto GenericCastToPtrInst = BI->getType()->getPointerStorageClass();
  switch (GenericCastToPtrInst) {
  case StorageClassCrossWorkgroup:
    return std::string(kOCLBuiltinName::ToGlobal);
  case StorageClassWorkgroup:
    return std::string(kOCLBuiltinName::ToLocal);
  case StorageClassFunction:
    return std::string(kOCLBuiltinName::ToPrivate);
  default:
    llvm_unreachable("Invalid address space");
    return "";
  }
}

llvm::GlobalValue::LinkageTypes
SPIRVToLLVM::transLinkageType(const SPIRVValue *V) {
  if (V->getLinkageType() == LinkageTypeInternal) {
    if (V->getOpCode() == OpVariable) {
      // Variable declaration
      SPIRVStorageClassKind StorageClass =
        static_cast<const SPIRVVariable*>(V)->getStorageClass();
      if (StorageClass == StorageClassUniformConstant ||
          StorageClass == StorageClassInput ||
          StorageClass == StorageClassUniform ||
          StorageClass == StorageClassPushConstant ||
          StorageClass == StorageClassStorageBuffer)
        return GlobalValue::ExternalLinkage;
      else if (StorageClass == StorageClassPrivate ||
               StorageClass == StorageClassOutput)
        return GlobalValue::PrivateLinkage;
    }
    return GlobalValue::InternalLinkage;
  } else if (V->getLinkageType() == LinkageTypeImport) {
    // Function declaration
    if (V->getOpCode() == OpFunction) {
      if (static_cast<const SPIRVFunction *>(V)->getNumBasicBlock() == 0)
        return GlobalValue::ExternalLinkage;
    }
    // Variable declaration
    if (V->getOpCode() == OpVariable) {
      if (static_cast<const SPIRVVariable *>(V)->getInitializer() == 0)
        return GlobalValue::ExternalLinkage;
    }
    // Definition
    return GlobalValue::AvailableExternallyLinkage;
  } else { // LinkageTypeExport
    if (V->getOpCode() == OpVariable) {
      if (static_cast<const SPIRVVariable *>(V)->getInitializer() == 0)
        // Tentative definition
        return GlobalValue::CommonLinkage;
    }
    return GlobalValue::ExternalLinkage;
  }
}

Instruction *SPIRVToLLVM::transOCLAllAny(SPIRVInstruction *I, BasicBlock *BB) {
  CallInst *CI = cast<CallInst>(transSPIRVBuiltinFromInst(I, BB));
  AttributeList Attrs = CI->getCalledFunction()->getAttributes();
  return cast<Instruction>(mapValue(
      I, mutateCallInstOCL(
             M, CI,
             [=](CallInst *, std::vector<Value *> &Args, llvm::Type *&RetTy) {
               Type *Int32Ty = Type::getInt32Ty(*Context);
               auto OldArg = CI->getOperand(0);
               auto NewArgTy = VectorType::get(
                   Int32Ty, OldArg->getType()->getVectorNumElements());
               auto NewArg =
                   CastInst::CreateSExtOrBitCast(OldArg, NewArgTy, "", CI);
               Args[0] = NewArg;
               RetTy = Int32Ty;
               return CI->getCalledFunction()->getName();
             },
             [=](CallInst *NewCI) -> Instruction * {
               return CastInst::CreateTruncOrBitCast(
                   NewCI, Type::getInt1Ty(*Context), "", NewCI->getNextNode());
             },
             &Attrs)));
}

Instruction *SPIRVToLLVM::transOCLRelational(SPIRVInstruction *I,
                                             BasicBlock *BB) {
  CallInst *CI = cast<CallInst>(transSPIRVBuiltinFromInst(I, BB));
  AttributeList Attrs = CI->getCalledFunction()->getAttributes();
  return cast<Instruction>(mapValue(
      I, mutateCallInstOCL(
             M, CI,
             [=](CallInst *, std::vector<Value *> &Args, llvm::Type *&RetTy) {
               Type *IntTy = Type::getInt32Ty(*Context);
               RetTy = Type::getInt1Ty(*Context);
               if (CI->getType()->isVectorTy())
                 RetTy =
                     VectorType::get(Type::getInt1Ty(*Context),
                                     CI->getType()->getVectorNumElements());
               return CI->getCalledFunction()->getName();
             },
             [=](CallInst *NewCI) -> Instruction * {
               Type *RetTy = Type::getInt1Ty(*Context);
               if (NewCI->getType()->isVectorTy())
                 RetTy =
                     VectorType::get(Type::getInt1Ty(*Context),
                                     NewCI->getType()->getVectorNumElements());
               return CastInst::CreateTruncOrBitCast(NewCI, RetTy, "",
                                                     NewCI->getNextNode());
             },
             &Attrs)));
}

// Widen i1 or vector of i1 type to i32 or vector of i32.
// We use this to represent bool or vector of bool as i1 normally, but as i32
// if it is stored in memory or in a struct or array, to avoid the problem that
// LLVM does not support GEP into vector of i1.
Type *SPIRVToLLVM::widenBoolType(Type *Ty) {
  if (auto ITy = dyn_cast<IntegerType>(Ty))
    if (ITy->getBitWidth() == 1)
      return Type::getInt32Ty(*Context);
  if (auto VTy = dyn_cast<VectorType>(Ty))
    if (auto ITy = dyn_cast<IntegerType>(VTy->getElementType()))
      if (ITy->getBitWidth() == 1)
        return VectorType::get(Type::getInt32Ty(*Context), VTy->getNumElements());
  return Ty;
}

// Widen i1 or vector of i1 value to i32 or vector of i32.
Value *SPIRVToLLVM::widenBoolValue(Value *V, BasicBlock *BB) {
  auto Ty = V->getType();
  auto WideTy = widenBoolType(V->getType());
  if (WideTy == Ty)
    return V;
  return CastInst::Create(Instruction::ZExt, V, WideTy, "", BB);
}

// Widen constant i1 or vector of i1 value to i32 or vector of i32.
Constant *SPIRVToLLVM::widenBoolConstant(Constant *C) {
  auto Ty = C->getType();
  auto WideTy = widenBoolType(C->getType());
  if (WideTy == Ty)
    return C;
  return ConstantExpr::getCast(Instruction::ZExt, C, WideTy);
}

// Narrow i32 or vector of i32 representing a bool value to i1 or vector of i1.
Value *SPIRVToLLVM::narrowBoolValue(Value *V, SPIRVType *BT, BasicBlock *BB) {
  auto Ty = V->getType();
  auto NarrowTy = transType(BT);
  if (Ty == NarrowTy)
    return V;
  return CastInst::Create(Instruction::Trunc, V, NarrowTy, "", BB);
}

} // namespace SPIRV

bool llvm::readSpirv(LLVMContext &C, std::istream &IS,
                     spv::ExecutionModel EntryExecModel, const char *EntryName,
                     const SPIRVSpecConstMap &SpecConstMap, Module *&M,
                     std::string &ErrMsg) {
  M = new Module("", C);
  std::unique_ptr<SPIRVModule> BM(SPIRVModule::createSPIRVModule());

  IS >> *BM;

  SPIRVToLLVM BTL(M, BM.get(), SpecConstMap);
  bool Succeed = true;
  if (!BTL.translate(EntryExecModel, EntryName)) {
    BM->getError(ErrMsg);
    Succeed = false;
  }
  llvm::legacy::PassManager PassMgr;
  PassMgr.add(createSPIRVToOCL20());
  PassMgr.run(*M);

  if (DbgSaveTmpLLVM)
    dumpLLVM(M, DbgTmpLLVMFileName);
  if (!Succeed) {
    delete M;
    M = nullptr;
  }
  return Succeed;
}

