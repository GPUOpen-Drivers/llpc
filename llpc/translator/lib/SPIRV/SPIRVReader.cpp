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
#include "SPIRVBasicBlock.h"
#include "SPIRVExtInst.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "SPIRVInternal.h"
#include "SPIRVModule.h"
#include "SPIRVType.h"
#include "SPIRVUtil.h"
#include "SPIRVValue.h"

#include "llpcBuilder.h"
#include "llpcCompiler.h"
#include "llpcContext.h"
#include "llpcPipeline.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/CFG.h"
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
#include "llvm/IR/ValueMap.h"
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

#define DEBUG_TYPE "spirv"

using namespace std;
using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace SPIRV {

cl::opt<bool> SPIRVGenFastMath("spirv-gen-fast-math",
    cl::init(true), cl::desc("Enable fast math mode with generating floating"
                              "point binary ops"));

cl::opt<bool> SPIRVWorkaroundBadSPIRV("spirv-workaround-bad-spirv",
    cl::init(true), cl::desc("Enable workarounds for bad SPIR-V"));

// Prefix for placeholder global variable name.
const char *KPlaceholderPrefix = "placeholder.";

// Prefix for row major matrix helpers.
static const char SpirvLaunderRowMajor[] = "spirv.launder.row_major";

static const SPIRVWord SPV_VERSION_1_0 = 0x00010000;

// Save the translated LLVM before validation for debugging purpose.
static bool DbgSaveTmpLLVM = false;
static const char *DbgTmpLLVMFileName = "_tmp_llvmbil.ll";

typedef std::pair<unsigned, AttributeList> AttributeWithIndex;

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

class SPIRVToLLVMDbgTran {
public:
  SPIRVToLLVMDbgTran(SPIRVModule *TBM, Module *TM)
      : BM(TBM), M(TM), SpDbg(BM), Builder(*M) {
    Enable = BM->hasDebugInfo();
  }

  void createCompileUnit() {
    if (!Enable)
      return;
    auto File = SpDbg.getEntryPointFileStr(ExecutionModelVertex, 0);
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
      auto SPFlags = DISubprogram::SPFlagDefinition;
      if (Function::isInternalLinkage(F->getLinkage()))
        SPFlags |= DISubprogram::SPFlagLocalToUnit;
      return Builder.createFunction(
          DF, FN, FN, DF, LN,
          Builder.createSubroutineType(Builder.getOrCreateTypeArray(None)),
          LN, DINode::FlagZero, SPFlags);
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
    const SPIRVSpecConstMap &TheSpecConstMap, Builder *pBuilder, const ShaderModuleUsage* pModuleUsage)
    :M(LLVMModule), m_pBuilder(pBuilder), BM(TheSPIRVModule),
    EnableXfb(false), EntryTarget(nullptr),
    SpecConstMap(TheSpecConstMap), DbgTran(BM, M),
    ModuleUsage(reinterpret_cast<const ShaderModuleUsage*>(pModuleUsage)) {
    assert(M);
    Context = &M->getContext();
  }

  DebugLoc getDebugLoc(SPIRVInstruction *BI, Function *F);

  void updateBuilderDebugLoc(SPIRVValue *BV, Function *F);

  Type *transType(SPIRVType *BT, uint32_t MatrixStride = 0,
    bool ColumnMajor = true, bool ParentIsPointer = false,
    bool ExplicitlyLaidOut = false);
  template<spv::Op> Type* transTypeWithOpcode(SPIRVType *BT,
    uint32_t MatrixStride, bool ColumnMajor, bool ParentIsPointer,
    bool ExplicitlyLaidOut);
  std::vector<Type *> transTypeVector(const std::vector<SPIRVType *> &);
  bool translate(ExecutionModel EntryExecModel, const char *EntryName);
  bool transAddressingModel();

  Value *transValue(SPIRVValue *, Function *F, BasicBlock *,
                    bool CreatePlaceHolder = true);
  Value *transValueWithoutDecoration(SPIRVValue *, Function *F, BasicBlock *,
                                     bool CreatePlaceHolder = true);
  Value *transAtomicRMW(SPIRVValue*, const AtomicRMWInst::BinOp);
  Constant* transInitializer(SPIRVValue*, Type*);
  template<spv::Op> Value *transValueWithOpcode(SPIRVValue*);
  Value *transLoadImage(SPIRVValue *SpvImageLoadPtr);
  Value *transImagePointer(SPIRVValue* SpvImagePtr);
  Value *transOpAccessChainForImage(SPIRVAccessChainBase* SpvAccessChain);
  Value *indexDescPtr(Value *Base, Value *Index, bool IsNonUniform,
                      SPIRVType *SpvElementType);
  Value* transGroupArithOp(Builder::GroupArithOp, SPIRVValue*);

  bool transDecoration(SPIRVValue *, Value *);
  bool transShaderDecoration(SPIRVValue *, Value *);
  bool checkContains64BitType(SPIRVType *BT);
  Constant *buildShaderInOutMetadata(SPIRVType *BT, ShaderInOutDecorate &InOutDec, Type *&MetaTy);
  Constant *buildShaderBlockMetadata(SPIRVType* BT, ShaderBlockDecorate &BlockDec, Type *&MDTy);
  uint32_t calcShaderBlockSize(SPIRVType *BT, uint32_t BlockSize, uint32_t MatrixStride, bool IsRowMajor);
  Value *transGLSLExtInst(SPIRVExtInst *ExtInst, BasicBlock *BB);
  Value *flushDenorm(Value *Val);
  Value *transTrinaryMinMaxExtInst(SPIRVExtInst *ExtInst, BasicBlock *BB);
  Value *transGLSLBuiltinFromExtInst(SPIRVExtInst *BC, BasicBlock *BB);
  std::vector<Value *> transValue(const std::vector<SPIRVValue *> &,
                                  Function *F, BasicBlock *);
  Function *transFunction(SPIRVFunction *F);
  bool transMetadata();
  bool transNonTemporalMetadata(Instruction *I);
  Value *transConvertInst(SPIRVValue *BV, Function *F, BasicBlock *BB);
  Instruction *transBuiltinFromInst(const std::string &FuncName,
                                    SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transSPIRVBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB);
  Instruction *transBarrierFence(SPIRVInstruction *BI, BasicBlock *BB);

  // Struct used to pass information in and out of getImageDesc.
  struct ExtractedImageInfo {
    BasicBlock *BB;
    const SPIRVTypeImageDescriptor *Desc;
    unsigned Dim;   // Llpc::Builder dimension
    unsigned Flags; // Llpc::Builder image call flags
    Value *ImageDesc;
    Value *FmaskDesc;
    Value *SamplerDesc;
  };

  // Load image and/or sampler descriptors, and get information from the image
  // type.
  void getImageDesc(SPIRVValue *BImageInst, ExtractedImageInfo *Info);

  // Set up address operand array for image sample/gather builder call.
  void setupImageAddressOperands(SPIRVInstruction *BI, unsigned MaskIdx,
                                 bool HasProj, MutableArrayRef<Value *> Addr,
                                 ExtractedImageInfo *ImageInfo, Value **SampleNum);

  // Handle fetch/read/write/atomic aspects of coordinate.
  void handleImageFetchReadWriteCoord(SPIRVInstruction *BI,
                                      ExtractedImageInfo *ImageInfo,
                                      MutableArrayRef<Value *> Addr,
                                      bool EnableMultiView = true);

  // Translate SPIR-V image atomic operations to LLVM function calls
  Value *transSPIRVImageAtomicOpFromInst(SPIRVInstruction *BI, BasicBlock *BB);

  // Translates SPIR-V fragment mask operations to LLVM function calls
  Value *transSPIRVFragmentMaskFetchFromInst(SPIRVInstruction *BI,
                                             BasicBlock *BB);
  Value *transSPIRVFragmentFetchFromInst(SPIRVInstruction *BI,
                                         BasicBlock *BB);

  // Translate image sample to LLVM IR
  Value *transSPIRVImageSampleFromInst(SPIRVInstruction *BI, BasicBlock *BB);

  // Translate image gather to LLVM IR
  Value *transSPIRVImageGatherFromInst(SPIRVInstruction *BI, BasicBlock *BB);

  // Translate image fetch/read to LLVM IR
  Value *transSPIRVImageFetchReadFromInst(SPIRVInstruction *BI, BasicBlock *BB);

  // Translate image write to LLVM IR
  Value *transSPIRVImageWriteFromInst(SPIRVInstruction *BI, BasicBlock *BB);

  // Translate OpImageQueryLevels to LLVM IR
  Value *transSPIRVImageQueryLevelsFromInst(SPIRVInstruction *BI,
                                            BasicBlock *BB);

  // Translate OpImageQuerySamples to LLVM IR
  Value *transSPIRVImageQuerySamplesFromInst(SPIRVInstruction *BI,
                                             BasicBlock *BB);

  // Translate OpImageQuerySize/OpImageQuerySizeLod to LLVM IR
  Value *transSPIRVImageQuerySizeFromInst(SPIRVInstruction *BI, BasicBlock *BB);

  // Translate OpImageQueryLod to LLVM IR
  Value *transSPIRVImageQueryLodFromInst(SPIRVInstruction *BI, BasicBlock *BB);

  Value* createLaunderRowMajorMatrix(Value* const);
  Value* addLoadInstRecursively(SPIRVType* const, Value* const, bool, bool, bool);
  void addStoreInstRecursively(SPIRVType* const, Value* const, Value* const, bool, bool, bool);
  Constant* buildConstStoreRecursively(SPIRVType* const, Type* const, Constant* const);

  // Post-process translated LLVM module to undo row major matrices.
  bool postProcessRowMajorMatrix();

  typedef DenseMap<SPIRVType *, Type *> SPIRVToLLVMTypeMap;
  typedef DenseMap<SPIRVValue *, Value *> SPIRVToLLVMValueMap;
  typedef DenseMap<SPIRVValue *, Value *> SPIRVBlockToLLVMStructMap;
  typedef DenseMap<SPIRVFunction *, Function *> SPIRVToLLVMFunctionMap;
  typedef DenseMap<GlobalVariable *, SPIRVBuiltinVariableKind> BuiltinVarMap;
  typedef DenseMap<SPIRVType *, SmallVector<uint32_t, 8>> RemappedTypeElementsMap;

  // A SPIRV value may be translated to a load instruction of a placeholder
  // global variable. This map records load instruction of these placeholders
  // which are supposed to be replaced by the real values later.
  typedef std::map<SPIRVValue *, LoadInst *> SPIRVToLLVMPlaceholderMap;

private:
  Module *M;
  BuiltinVarMap BuiltinGVMap;
  LLVMContext *Context;
  Builder *m_pBuilder;
  SPIRVModule *BM;
  bool EnableXfb;
  bool EnableGatherLodNz;
  ShaderFloatControlFlags FpControlFlags;
  SPIRVFunction* EntryTarget;
  const SPIRVSpecConstMap &SpecConstMap;
  SPIRVToLLVMTypeMap TypeMap;
  SPIRVToLLVMValueMap ValueMap;
  SPIRVToLLVMFunctionMap FuncMap;
  SPIRVBlockToLLVMStructMap BlockMap;
  SPIRVToLLVMPlaceholderMap PlaceholderMap;
  SPIRVToLLVMDbgTran DbgTran;
  std::map<std::string, uint32_t> MangleNameToIndex;
  RemappedTypeElementsMap RemappedTypeElements;
  DenseMap<Type *, bool> TypesWithPadMap;
  DenseMap<Type*, uint64_t> TypeToStoreSize;
  DenseMap<std::pair<SPIRVType*, uint32_t>, Type *> OverlappingStructTypeWorkaroundMap;
  DenseMap<std::pair<BasicBlock*, BasicBlock*>, unsigned> BlockPredecessorToCount;
  const ShaderModuleUsage* ModuleUsage;

  Llpc::Builder *getBuilder() const { return m_pBuilder; }

  Type *mapType(SPIRVType *BT, Type *T) {
    TypeMap[BT] = T;
    return T;
  }

  void recordRemappedTypeElements(SPIRVType *BT, uint32_t From, uint32_t To) {
    auto& Elements = RemappedTypeElements[BT];

    if (Elements.size() <= From) {
      Elements.resize(From + 1, 0);
    }

    Elements[From] = To;
  }

  bool isRemappedTypeElements(SPIRVType *BT) const {
    return RemappedTypeElements.count(BT) > 0;
  }

  uint32_t lookupRemappedTypeElements(SPIRVType *BT, uint32_t From) {
    assert(RemappedTypeElements.count(BT) > 0);
    assert(RemappedTypeElements[BT].size() > From);
    return RemappedTypeElements[BT][From];
  }

  Type *getPadType(uint32_t bytes) {
    return ArrayType::get(getBuilder()->getInt8Ty(), bytes);
  }

  Type *recordTypeWithPad(Type* const T, bool isMatrixRow = false) {
    TypesWithPadMap[T] = isMatrixRow;
    return T;
  }

  bool isTypeWithPad(Type* const T) const {
    return TypesWithPadMap.count(T) > 0;
  }

  bool isTypeWithPadRowMajorMatrix(Type* const T) const {
    return TypesWithPadMap.lookup(T);
  }

  // Returns a cached type store size. If there is no entry for the given type,
  // its store size is calculated and added to the cache.
  uint64_t getTypeStoreSize(Type* const T) {
    auto it = TypeToStoreSize.find(T);
    if (it != TypeToStoreSize.end()) {
      return it->second;
    }

    const uint64_t calculatedSize = M->getDataLayout().getTypeStoreSize(T);
    TypeToStoreSize[T] = calculatedSize;
    return calculatedSize;
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

  // Used to keep track of the number of incoming edges to a block from each
  // of the predecessor.
  void recordBlockPredecessor(BasicBlock* Block, BasicBlock* PredecessorBlock) {
    assert(Block);
    assert(PredecessorBlock);
    BlockPredecessorToCount[{Block, PredecessorBlock}] += 1;
  }

  unsigned
  getBlockPredecessorCounts(BasicBlock* Block, BasicBlock* Predecessor) {
    assert(Block);
    // This will create the map entry if it does not already exist.
    auto it = BlockPredecessorToCount.find({Block, Predecessor});
    if (it != BlockPredecessorToCount.end())
      return it->second;

    return 0;
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
  // Change this if it is no longer true.
  bool isFuncNoUnwind() const { return true; }
  bool isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction *BI) const;

  Value *mapFunction(SPIRVFunction *BF, Function *F) {
    FuncMap[BF] = F;
    return F;
  }

  Value *getTranslatedValue(SPIRVValue *BV);

  SPIRVErrorLog &getErrorLog() { return BM->getErrorLog(); }

  void setCallingConv(CallInst *Call) {
    Function *F = Call->getCalledFunction();
    assert(F);
    Call->setCallingConv(F->getCallingConv());
  }

  void setAttrByCalledFunc(CallInst *Call);
  Type *transFPType(SPIRVType *T);
  void setFastMathFlags(SPIRVValue *BV);
  void setFastMathFlags(Value *Val);
  BinaryOperator *transShiftLogicalBitwiseInst(SPIRVValue *BV, BasicBlock *BB,
                                               Function *F);
  Instruction *transCmpInst(SPIRVValue *BV, BasicBlock *BB, Function *F);

  void setName(llvm::Value *V, SPIRVValue *BV);
  void setLLVMLoopMetadata(SPIRVLoopMerge *LM, BranchInst *BI);
  template <class Source, class Func> bool foreachFuncCtlMask(Source, Func);
  llvm::GlobalValue::LinkageTypes transLinkageType(const SPIRVValue *V);

  Instruction *transBarrier(BasicBlock *BB, SPIRVWord ExecScope,
                               SPIRVWord MemSema, SPIRVWord MemScope);

  Instruction *transMemFence(BasicBlock *BB, SPIRVWord MemSema,
                             SPIRVWord MemScope);
  void truncConstantIndex(std::vector<Value*> &Indices, BasicBlock *BB);
};

Value *SPIRVToLLVM::getTranslatedValue(SPIRVValue *BV) {
  auto Loc = ValueMap.find(BV);
  if (Loc != ValueMap.end())
    return Loc->second;
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

// =====================================================================================================================
// Translate an "OpTypeArray". This contains special handling for arrays in interface storage classes which are
// explicitly laid out and may contain manually placed padding bytes. If the array needs padding, we map an array like
// '<element>[length]' -> 'struct { <element>, <padding bytes> }[length]'.
template<> Type *SPIRVToLLVM::transTypeWithOpcode<spv::OpTypeArray>(
    SPIRVType* const pSpvType,            // [in] The type.
    const uint32_t   matrixStride,        // The matrix stride (can be 0).
    const bool       isColumnMajor,       // Whether the matrix is column major.
    const bool       isParentPointer,     // If the parent is a pointer type.
    const bool       isExplicitlyLaidOut) // If the type is one which is explicitly laid out.
{
    Type* pElementType = transType(pSpvType->getArrayElementType(),
                                   matrixStride,
                                   isColumnMajor,
                                   isParentPointer,
                                   isExplicitlyLaidOut);

    SPIRVWord arrayStride = 0;
    const bool hasArrayStride = pSpvType->hasDecorate(DecorationArrayStride, 0, &arrayStride);
    assert(hasArrayStride ^ (arrayStride == 0));

    const uint64_t storeSize = getTypeStoreSize(pElementType);

    bool paddedArray = false;

    if (isExplicitlyLaidOut && hasArrayStride)
    {
        assert(arrayStride >= storeSize);

        const uint32_t padding = static_cast<uint32_t>(arrayStride - storeSize);

        paddedArray = padding > 0;

        if (paddedArray)
        {
            // Record that the array was remapped, even though we don't record a useful mapping for arrays.
            recordRemappedTypeElements(pSpvType, 0, 0);

            pElementType = StructType::create({ pElementType, getPadType(padding) }, "llpc.array.element", true);
        }
    }

    Type* const pArrayType = ArrayType::get(pElementType, pSpvType->getArrayLength());
    return paddedArray ? recordTypeWithPad(pArrayType) : pArrayType;
}

// =====================================================================================================================
// Translate an "OpTypeBool". This contains special handling for bools in pointers, which we need to map separately
// because boolean values in memory are represented as i32.
template<> Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeBool>(
    SPIRVType* const pSpvType,            // [in] The type.
    const uint32_t   matrixStride,        // The matrix stride (can be 0).
    const bool       isColumnMajor,       // Whether the matrix is column major.
    const bool       isParentPointer,     // If the parent is a pointer type.
    const bool       isExplicitlyLaidOut) // If the type is one which is explicitly laid out.
{
    if (isParentPointer)
    {
        return getBuilder()->getInt32Ty();
    }
    else
    {
        return getBuilder()->getInt1Ty();
    }
}

// =====================================================================================================================
// Translate an "OpTypeForwardPointer".
template<> Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeForwardPointer>(
    SPIRVType* const pSpvType,            // [in] The type.
    const uint32_t   matrixStride,        // The matrix stride (can be 0).
    const bool       isColumnMajor,       // Whether the matrix is column major.
    const bool       isParentPointer,     // If the parent is a pointer type.
    const bool       isExplicitlyLaidOut) // If the type is one which is explicitly laid out.
{
    SPIRVTypeForwardPointer* const pSpvForwardPointerType = static_cast<SPIRVTypeForwardPointer*>(pSpvType);
    const SPIRVStorageClassKind storageClass = pSpvForwardPointerType->getPointerStorageClass();

    // Forward pointers must always point to structs.
    assert(pSpvForwardPointerType->getPointerElementType()->isTypeStruct());

    // We first have to map the pointed-to-struct to an opaque struct so we can have a forward reference to the struct.
    StructType* const pPointeeType = StructType::create(*Context);

    // Then we need to map our forward pointer itself, because the struct we are pointing to could use the pointer.
    const uint32_t addrSpace = SPIRSPIRVAddrSpaceMap::rmap(storageClass);
    Type* const pType = mapType(pSpvType, PointerType::get(pPointeeType, addrSpace));

    const bool isBufferBlockPointer = (storageClass == StorageClassStorageBuffer) ||
                                      (storageClass == StorageClassUniform) ||
                                      (storageClass == StorageClassPushConstant) ||
                                      (storageClass == StorageClassPhysicalStorageBufferEXT);

    // Finally we translate the struct we are pointing to to create it.
    StructType* const pStructType = cast<StructType>(transType(pSpvType->getPointerElementType(),
                                                     matrixStride,
                                                     isColumnMajor,
                                                     true,
                                                     isBufferBlockPointer));

    pPointeeType->setBody(pStructType->elements(), pStructType->isPacked());

    return pType;
}

// =====================================================================================================================
// Translate an "OpTypeMatrix". This contains special handling for matrices in interface storage classes which are
// explicitly laid out and may contain manually placed padding bytes after the column elements.
template<> Type* SPIRVToLLVM::transTypeWithOpcode<OpTypeMatrix>(
    SPIRVType* const pSpvType,            // [in] The type.
    uint32_t         matrixStride,        // The matrix stride (can be 0).
    const bool       isColumnMajor,       // Whether the matrix is column major.
    const bool       isParentPointer,     // If the parent is a pointer type.
    const bool       isExplicitlyLaidOut) // If the type is one which is explicitly laid out.
{
    Type* pColumnType = nullptr;

    uint32_t columnCount = pSpvType->getMatrixColumnCount();

    // If the matrix is not explicitly laid out or is column major, just translate the column type.
    if ((isParentPointer == false) || isColumnMajor)
    {
        pColumnType = transType(pSpvType->getMatrixColumnType(),
                                matrixStride,
                                isColumnMajor,
                                isParentPointer,
                                isExplicitlyLaidOut);
    }
    else
    {
        // We need to transpose the matrix type to represent its layout in memory.
        SPIRVType* const pSpvColumnType = pSpvType->getMatrixColumnType();

        Type* const pElementType = transType(pSpvColumnType->getVectorComponentType(),
                                             matrixStride,
                                             isColumnMajor,
                                             isParentPointer,
                                             isExplicitlyLaidOut);

        pColumnType = ArrayType::get(pElementType, columnCount);
        columnCount = pSpvColumnType->getVectorComponentCount();

        if ((isColumnMajor == false) && (matrixStride == 0))
        {
            // Targeted for std430 layout
            assert(columnCount == 4);
            matrixStride = columnCount * (pElementType->getPrimitiveSizeInBits()/ 8);
        }
    }

    const bool isPaddedMatrix = matrixStride > 0;

    if (isExplicitlyLaidOut && isPaddedMatrix)
    {
        SmallVector<Type*, 2> memberTypes;

        memberTypes.push_back(pColumnType);

        const uint64_t storeSize = getTypeStoreSize(pColumnType);
        assert(matrixStride >= storeSize);

        const uint32_t padding = static_cast<uint32_t>(matrixStride - storeSize);

        if (padding > 0)
        {
            memberTypes.push_back(getPadType(padding));
        }

        const StringRef typeName = isColumnMajor ? "llpc.matrix.column" : "llpc.matrix.row";

        pColumnType = StructType::create(memberTypes, typeName, true);
    }

    Type* const pMatrixType = ArrayType::get(pColumnType, columnCount);
    return (isExplicitlyLaidOut && isPaddedMatrix) ?
           recordTypeWithPad(pMatrixType, isColumnMajor == false) :
           pMatrixType;
}

// =====================================================================================================================
// Translate an "OpTypePointer". This contains special handling for pointers to bool, which we need to map separately
// because boolean values in memory are represented as i32, and special handling for images and samplers.
template<> Type *SPIRVToLLVM::transTypeWithOpcode<OpTypePointer>(
    SPIRVType* const pSpvType,            // [in] The type.
    const uint32_t   matrixStride,        // The matrix stride (can be 0).
    const bool       isColumnMajor,       // Whether the matrix is column major.
    const bool       isParentPointer,     // If the parent is a pointer type.
    const bool       isExplicitlyLaidOut) // If the type is one which is explicitly laid out.
{
    const SPIRVStorageClassKind storageClass = pSpvType->getPointerStorageClass();

    // Handle image etc types first, if in UniformConstant memory.
    if (storageClass == StorageClassUniformConstant)
    {
        auto pSpvElementType = pSpvType->getPointerElementType();
        while ((pSpvElementType->getOpCode() == OpTypeArray) || (pSpvElementType->getOpCode() == OpTypeRuntimeArray))
        {
            // Pointer to array (or runtime array) of image/sampler/sampledimage has the same representation as
            // a simple pointer to same image/sampler/sampledimage.
            pSpvElementType = pSpvElementType->getArrayElementType();
        }

        switch (pSpvElementType->getOpCode())
        {
        case OpTypeImage:
            {
                if (static_cast<SPIRVTypeImage*>(pSpvElementType)->getDescriptor().MS)
                {
                    // Pointer to multisampled image is represented by a struct containing pointer to
                    // normal image descriptor and pointer to fmask descriptor.
                    return StructType::get(*Context,
                                           { getBuilder()->GetImageDescPtrTy(), getBuilder()->GetFmaskDescPtrTy() });
                }
                if (static_cast<SPIRVTypeImage*>(pSpvElementType)->getDescriptor().Dim == DimBuffer)
                {
                    return getBuilder()->GetTexelBufferDescPtrTy();
                }
                return getBuilder()->GetImageDescPtrTy();
            }
        case OpTypeSampler:
            {
                return getBuilder()->GetSamplerDescPtrTy();
            }
        case OpTypeSampledImage:
            {
                // Pointer to sampled image is represented by a struct containing pointer to image and pointer to
                // sampler. But if the image is multisampled, then it itself is another struct, as above.
                Type* pImagePtrTy = getBuilder()->GetImageDescPtrTy();
                SPIRVTypeImage* pSpvImageTy = static_cast<SPIRVTypeSampledImage*>(pSpvElementType)->getImageType();
                if (pSpvImageTy->getDescriptor().Dim == DimBuffer)
                {
                    pImagePtrTy = getBuilder()->GetTexelBufferDescPtrTy();
                }
                else if (pSpvImageTy->getDescriptor().MS)
                {
                    pImagePtrTy = StructType::get(*Context,
                                                  { getBuilder()->GetImageDescPtrTy(), getBuilder()->GetFmaskDescPtrTy() });
                }
                return StructType::get(*Context, { pImagePtrTy, getBuilder()->GetSamplerDescPtrTy() });
            }
        default:
            {
                break;
            }
        }
    }

    // Now non-image-related handling.
    const bool explicitlyLaidOut =
        (storageClass == StorageClassStorageBuffer) ||
        (storageClass == StorageClassUniform) ||
        (storageClass == StorageClassPushConstant) ||
        (storageClass == StorageClassPhysicalStorageBufferEXT);

    Type* const pPointeeType = transType(pSpvType->getPointerElementType(),
                                         matrixStride,
                                         isColumnMajor,
                                         true,
                                         explicitlyLaidOut);

    return PointerType::get(pPointeeType, SPIRSPIRVAddrSpaceMap::rmap(storageClass));
}

// =====================================================================================================================
// Translate an "OpTypeRuntimeArray". This contains special handling for arrays in interface storage classes which are
// explicitly laid out and may contain manually placed padding bytes. If the array needs padding, we map an array like
// '<element>[length]' -> 'struct { <element>, <padding bytes> }[length]'.
template<> Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeRuntimeArray>(
    SPIRVType* const pSpvType,            // [in] The type.
    const uint32_t   matrixStride,        // The matrix stride (can be 0).
    const bool       isColumnMajor,       // Whether the matrix is column major.
    const bool       isParentPointer,     // If the parent is a pointer type.
    const bool       isExplicitlyLaidOut) // If the type is one which is explicitly laid out.
{
    Type* pElementType = transType(pSpvType->getArrayElementType(),
                                   matrixStride,
                                   isColumnMajor,
                                   isParentPointer,
                                   isExplicitlyLaidOut);

    SPIRVWord arrayStride = 0;
    const bool hasArrayStride = pSpvType->hasDecorate(DecorationArrayStride, 0, &arrayStride);
    assert(hasArrayStride ^ (arrayStride == 0));
    (void(hasArrayStride)); // unused

    const uint64_t storeSize = getTypeStoreSize(pElementType);

    bool paddedArray = false;

    if (isExplicitlyLaidOut && hasArrayStride)
    {
        assert(arrayStride >= storeSize);

        const uint32_t padding = static_cast<uint32_t>(arrayStride - storeSize);

        paddedArray = padding > 0;

        if (paddedArray)
        {
            // Record that the array was remapped, even though we don't record a useful mapping for arrays.
            recordRemappedTypeElements(pSpvType, 0, 0);

            pElementType = StructType::create({pElementType, getPadType(padding)}, "llpc.runtime.array.element", true);
        }
    }

    Type* const pRuntimeArrayType = ArrayType::get(pElementType, SPIRVWORD_MAX);
    return paddedArray ? recordTypeWithPad(pRuntimeArrayType) : pRuntimeArrayType;
}

// =====================================================================================================================
// Translate an "OpTypeStruct". This contains special handling for structures in interface storage classes which are
// explicitly laid out and may contain manually placed padding bytes between any struct elements (including perhaps
// before the first struct element!).
template<> Type *SPIRVToLLVM::transTypeWithOpcode<spv::OpTypeStruct>(
    SPIRVType* const pSpvType,            // [in] The type.
    const uint32_t   matrixStride,        // The matrix stride (can be 0).
    const bool       isColumnMajor,       // Whether the matrix is column major.
    const bool       isParentPointer,     // If the parent is a pointer type.
    const bool       isExplicitlyLaidOut) // If the type is one which is explicitly laid out.
{
    SPIRVTypeStruct* const pSpvStructType = static_cast<SPIRVTypeStruct*>(pSpvType);

    bool isPacked = false;

    bool hasMemberOffset = false;

    using StructMember = std::tuple<SPIRVWord, SPIRVWord>;

    SmallVector<StructMember, 8> structMembers;

    for (SPIRVWord i = 0, memberCount = pSpvStructType->getMemberCount(); i < memberCount; i++)
    {
        SPIRVWord offset = 0;

        // If we have a member decorate, we need to handle the struct carefully. To do this we use a packed LLVM struct
        // type with manually added byte array pads. We record all the remappings of original index -> new index that
        // have occurred so that we can fixup GEPs and insert/extract's later.
        if (isExplicitlyLaidOut)
        {
            const bool nextHasMemberOffset = pSpvStructType->hasMemberDecorate(i, DecorationOffset, 0, &offset);

            // If we did not find a member offset, check that we did not see any member offsets on other members.
            assert((hasMemberOffset == false) || nextHasMemberOffset);

            hasMemberOffset = nextHasMemberOffset;
        }

        structMembers.push_back(StructMember(i, offset));
    }

    // Sort the members by the offsets they have into the struct.
    llvm::sort(structMembers, [](const StructMember& left, const StructMember& right)
        {
            // First order by offsets.
            if (std::get<1>(left) < std::get<1>(right))
            {
                return true;
            }
            else if (std::get<1>(left) > std::get<1>(right))
            {
                return false;
            }
            else
            {
                return std::get<0>(left) < std::get<0>(right);
            }
        });

    SPIRVWord lastIndex = 0;
    uint64_t lastValidByte = 0;

    SmallVector<Type *, 16> memberTypes;

    for (const StructMember& structMember : structMembers)
    {
        const SPIRVWord index = std::get<0>(structMember);
        const SPIRVWord offset = std::get<1>(structMember);

        if (isExplicitlyLaidOut && hasMemberOffset)
        {
            // HLSL-derived shaders contain some (entirely valid) strange mappings for arrays that cannot be represented
            // in LLVM. This manifests as an offset for a struct member that overlaps the previous data in the struct.
            // To workaround this, we need to change the previous member in the struct to a pad array that we'll sort
            // out during access-chain and load/stores later.
            if (offset < lastValidByte)
            {
                // Get the previous last member in the struct.
                Type* const pLastMemberType = memberTypes.back();

                // Pop it from the member types.
                memberTypes.pop_back();

                // Get the size of the last member.
                const uint64_t bytes = getTypeStoreSize(pLastMemberType);

                // Push a pad type into the struct for the member we are having to remap.
                memberTypes.push_back(getPadType(offset - (lastValidByte - bytes)));

                // Remember the original type of the struct member which we need later.
                OverlappingStructTypeWorkaroundMap[std::make_pair(pSpvType, lastIndex)] = pLastMemberType;

                // And set the last valid byte to the offset since we've worked around this.
                lastValidByte = offset;
            }
            else
            {
                const uint32_t padding = static_cast<uint32_t>(offset - lastValidByte);

                if (padding > 0)
                {
                    memberTypes.push_back(getPadType(padding));
                }
            }

            recordRemappedTypeElements(pSpvStructType, index, memberTypes.size());

            // We always pack structs with explicit offsets.
            isPacked = true;
        }

        SPIRVType* const pSpvMemberType = pSpvStructType->getMemberType(index);

        SPIRVWord memberMatrixStride = 0;
        pSpvStructType->hasMemberDecorate(index, DecorationMatrixStride, 0, &memberMatrixStride);

        const bool memberIsColumnMajor = pSpvStructType->hasMemberDecorate(index, DecorationRowMajor) == false;

        // If our member is a matrix, check that only one of the specifiers is declared.
        if (isExplicitlyLaidOut && (memberMatrixStride > 0))
        {
            assert(memberIsColumnMajor ^ pSpvStructType->hasMemberDecorate(index, DecorationRowMajor));
        }

        Type* const pMemberType = transType(pSpvMemberType,
                                            memberMatrixStride,
                                            memberIsColumnMajor,
                                            isParentPointer,
                                            isExplicitlyLaidOut);

        lastValidByte = offset + getTypeStoreSize(pMemberType);

        memberTypes.push_back(pMemberType);

        lastIndex = index;
    }

    StructType* pStructType = nullptr;
    if (pSpvStructType->isLiteral())
    {
        pStructType = StructType::get(*Context, memberTypes, isPacked);
    }
    else
    {
        pStructType = StructType::create(*Context, pSpvStructType->getName());
        pStructType->setBody(memberTypes, isPacked);
    }

    return isExplicitlyLaidOut && hasMemberOffset ? recordTypeWithPad(pStructType) : pStructType;
}

// =====================================================================================================================
// Translate an "OpTypeVector". Vectors in interface storage classes are laid out using arrays because vectors in our
// target triple have implicit padding bytes for 3-element vector types, which does not work with relaxed block layout
// or scalar block layout. We translate these arrays back to vectors before load/store operations.
template<> Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeVector>(
    SPIRVType* const pSpvType,            // [in] The type.
    const uint32_t   matrixStride,        // The matrix stride (can be 0).
    const bool       isColumnMajor,       // Whether the matrix is column major.
    const bool       isParentPointer,     // If the parent is a pointer type.
    const bool       isExplicitlyLaidOut) // If the type is one which is explicitly laid out.
{
    Type* const pCompType = transType(pSpvType->getVectorComponentType(),
                                      matrixStride,
                                      isColumnMajor,
                                      isParentPointer,
                                      isExplicitlyLaidOut);

    // If the vector is in a pointer, we need to use an array to represent it because of LLVMs data layout rules.
    if (isExplicitlyLaidOut)
    {
        return ArrayType::get(pCompType, pSpvType->getVectorComponentCount());
    }
    else
    {
        return VectorType::get(pCompType, pSpvType->getVectorComponentCount());
    }
}

Type *SPIRVToLLVM::transType(
  SPIRVType *T, uint32_t MatrixStride, bool ColumnMajor,
  bool ParentIsPointer, bool ExplicitlyLaidOut) {
  // If the type is not a sub-part of a pointer or it is a forward pointer, we can look in the map.
  if ((ParentIsPointer == false) || T->isTypeForwardPointer()) {
    auto Loc = TypeMap.find(T);
    if (Loc != TypeMap.end())
      return Loc->second;
  }

  T->validate();
  switch (T->getOpCode()) {
  case OpTypeVoid:
    return mapType(T, Type::getVoidTy(*Context));
  case OpTypeInt:
    return mapType(T, Type::getIntNTy(*Context, T->getIntegerBitWidth()));
  case OpTypeFloat:
    return mapType(T, transFPType(T));
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
    if (ST->getDescriptor().MS) {
      // A multisampled image is represented by a struct containing both the
      // image descriptor and the fmask descriptor.
      return mapType(T,
                     StructType::get(*Context, {getBuilder()->GetImageDescTy(),
                                                getBuilder()->GetFmaskDescTy()}));
    }
    if (ST->getDescriptor().Dim == DimBuffer)
    {
        // A buffer image is represented by a texel buffer descriptor.
        return mapType(T, getBuilder()->GetTexelBufferDescTy());
    }
    // Otherwise, an image is represented by an image descriptor.
    return mapType(T, getBuilder()->GetImageDescTy());
  }
  case OpTypeSampler:
    return mapType(T, getBuilder()->GetSamplerDescTy());
  case OpTypeSampledImage: {
    // A sampledimage is represented by a struct containing the image descriptor
    // and the sampler descriptor.
    auto SIT = static_cast<SPIRVTypeSampledImage *>(T);
    return mapType(T,
                   StructType::get(*Context, {transType(SIT->getImageType()),
                                              getBuilder()->GetSamplerDescTy()}));
  }
#define HANDLE_OPCODE(op) case (op): {                                         \
    Type *NewTy = transTypeWithOpcode<op>(T, MatrixStride, ColumnMajor,        \
      ParentIsPointer, ExplicitlyLaidOut);                                     \
    return ParentIsPointer ? NewTy : mapType(T, NewTy);                        \
  }

  HANDLE_OPCODE(OpTypeArray);
  HANDLE_OPCODE(OpTypeBool);
  HANDLE_OPCODE(OpTypeForwardPointer);
  HANDLE_OPCODE(OpTypeMatrix);
  HANDLE_OPCODE(OpTypePointer);
  HANDLE_OPCODE(OpTypeRuntimeArray);
  HANDLE_OPCODE(OpTypeStruct);
  HANDLE_OPCODE(OpTypeVector);

#undef HANDLE_OPCODE

  default: {
    llvm_unreachable("Not implemented");
  }
  }
  return 0;
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
  return isCmpOpCode(OC);
}

void SPIRVToLLVM::setName(llvm::Value *V, SPIRVValue *BV) {
  auto Name = BV->getName();

  if (Name.empty()) {
    return;
  }

  if (V->hasName()) {
    return;
  }

  if (V->getType()->isVoidTy()) {
    return;
  }

  V->setName(Name);
}

void SPIRVToLLVM::setLLVMLoopMetadata(SPIRVLoopMerge *LM, BranchInst *BI) {
  if (!LM)
    return;
  llvm::MDString *Name = nullptr;
  auto Temp = MDNode::getTemporary(*Context, None);
  auto Self = MDNode::get(*Context, Temp.get());
  Self->replaceOperandWith(0, Self);
  std::vector<llvm::Metadata*> MDs;
  if (LM->getLoopControl() == LoopControlMaskNone) {
    BI->setMetadata("llvm.loop", Self);
    return;
  } else if (LM->getLoopControl() == LoopControlUnrollMask) {
    Name = llvm::MDString::get(*Context, "llvm.loop.unroll.full");
    MDs.push_back(Name);
  } else if (LM->getLoopControl() == LoopControlDontUnrollMask) {
    Name = llvm::MDString::get(*Context, "llvm.loop.unroll.disable");
    MDs.push_back(Name);
  }
#if SPV_VERSION >= 0x10400
  else if (LM->getLoopControl() & LoopControlPartialCountMask) {
    Name = llvm::MDString::get(*Context, "llvm.loop.unroll.count");
    MDs.push_back(Name);

    auto PartialCount = ConstantInt::get(Type::getInt32Ty(*Context),
                                         LM->getLoopControlParameters().at(0));
    MDs.push_back(ConstantAsMetadata::get(PartialCount));
  }
#endif

  if (LM->getLoopControl() & LoopControlDependencyInfiniteMask
    || (LM->getLoopControl() & LoopControlDependencyLengthMask)) {
    // TODO: DependencyInfinite probably mapped to llvm.loop.parallel_accesses with llvm.access.group
    // DependencyLength potentially useful but without llvm mappings
    return;
  }

#if  SPV_VERSION >= 0x10400
  if (LM->getLoopControl() & LoopControlIterationMultipleMask) {
    // TODO: Potentially useful but without llvm mappings
    return;
  }
  if ((LM->getLoopControl() & LoopControlMaxIterationsMask)
    || (LM->getLoopControl() & LoopControlMinIterationsMask)
    || (LM->getLoopControl() & LoopControlPeelCountMask)) {
    // No LLVM mapping and not too important
    return;
  }
#endif

  if (MDs.empty())
    return;

  SmallVector<llvm::Metadata *, 2> Metadata;
  Metadata.push_back(llvm::MDNode::get(*Context, Self));
  Metadata.push_back(llvm::MDNode::get(*Context, MDs));

  llvm::MDNode *Node = llvm::MDNode::get(*Context, Metadata);
  Node->replaceOperandWith(0, Node);
  BI->setMetadata("llvm.loop", Node);
}

Value *SPIRVToLLVM::transValue(SPIRVValue *BV, Function *F, BasicBlock *BB,
                               bool CreatePlaceHolder) {
  SPIRVToLLVMValueMap::iterator Loc = ValueMap.find(BV);

  if (Loc != ValueMap.end() && (!PlaceholderMap.count(BV) || CreatePlaceHolder))
    return Loc->second;

  BV->validate();

  auto V = transValueWithoutDecoration(BV, F, BB, CreatePlaceHolder);
  if (!V) {
    return nullptr;
  }
  setName(V, BV);
  if (!transDecoration(BV, V)) {
    assert(0 && "trans decoration fail");
    return nullptr;
  }

  return V;
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
    if (BB)
      return CastInst::Create(CO, Src, Dst, BV->getName(), BB);
    return ConstantExpr::getCast(CO, dyn_cast<Constant>(Src), Dst);
  }
}

// Decide whether to set fast math flags in Builder, just before generating
// code for BV. Decorations on BV may prevent us from setting some flags.
void SPIRVToLLVM::setFastMathFlags(SPIRVValue *BV) {
  // For floating-point operations, if "FastMath" is enabled, set the "FastMath"
  // flags on the handled instruction
  llvm::FastMathFlags FMF;
  if (SPIRVGenFastMath) {
    FMF.setAllowReciprocal();
    // Enable contraction when "NoContraction" decoration is not specified
    bool AllowContract = !BV || !BV->hasDecorate(DecorationNoContraction);
    // Do not set AllowContract or AllowReassoc if DenormFlushToZero is on, to
    // avoid an FP operation being simplified to a move that does not flush
    // denorms.
    if (FpControlFlags.DenormFlushToZero == 0) {
      FMF.setAllowContract(AllowContract);
      // AllowRessociation should be same with AllowContract
      FMF.setAllowReassoc(AllowContract);
    }
    // Enable "no NaN" and "no signed zeros" only if there isn't any floating point control flags
    if (FpControlFlags.U32All == 0) {
      FMF.setNoNaNs();
      FMF.setNoSignedZeros(AllowContract);
    }
  }
  getBuilder()->setFastMathFlags(FMF);
}

// Set fast math flags on just-generated instruction Val.
// This is only needed if the instruction was not generated by Builder, or using
// a Builder method that does not honor FMF such as CreateMinNum.
void SPIRVToLLVM::setFastMathFlags(Value *Val) {
  if (auto Inst = dyn_cast<Instruction>(Val)) {
    if (isa<FPMathOperator>(Inst))
      Inst->setFastMathFlags(getBuilder()->getFastMathFlags());
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
  setFastMathFlags(Inst);

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

// =====================================================================================================================
// Post process the module to remove row major matrix uses.
bool SPIRVToLLVM::postProcessRowMajorMatrix()
{
    SmallVector<Value*, 8> valuesToRemove;

    for (Function& func : M->functions())
    {
        if (func.getName().startswith(SpirvLaunderRowMajor) == false)
        {
            continue;
        }

        // Remember to remove the function later.
        valuesToRemove.push_back(&func);

        for (User* const pUser : func.users())
        {
            CallInst* const pCall = dyn_cast<CallInst>(pUser);

            assert(pCall != nullptr);

            // Remember to remove the call later.
            valuesToRemove.push_back(pCall);

            Value* const pMatrix = pCall->getArgOperand(0);
            Type* const pDestType = pCall->getType()->getPointerElementType();
            assert(pDestType->isArrayTy());

            const uint32_t columnCount = pDestType->getArrayNumElements();
            const uint32_t rowCount = pDestType->getArrayElementType()->getArrayNumElements();

            Type* const pMatrixElementType = pDestType->getArrayElementType()->getArrayElementType();

            llvm::ValueMap<Value*, Value*> valueMap;

            // Initially populate the map with just our matrix source.
            valueMap[pCall] = pMatrix;

            SmallVector<Value*, 8> workList(pCall->user_begin(), pCall->user_end());

            while (workList.empty() == false)
            {
                Value* const pValue = workList.pop_back_val();

                Instruction* const pInst = dyn_cast<Instruction>(pValue);
                assert(pInst != nullptr);

                getBuilder()->SetInsertPoint(pInst);

                // Remember to remove the instruction later.
                valuesToRemove.push_back(pInst);

                if (BitCastInst* const pBitCast = dyn_cast<BitCastInst>(pValue))
                {
                    // We need to handle bitcasts because we need to represent SPIR-V vectors in interface types
                    // (uniform, storagebuffer, pushconstant) as arrays because of alignment requirements. When we do a
                    // load/store of a vector we actually bitcast the array type to a vector, then do the load, so we
                    // need to handle these bitcasts here.

                    valueMap[pBitCast] = valueMap[pBitCast->getOperand(0)];

                    // Add all the users of this bitcast to the worklist for processing.
                    for (User* const pUser : pBitCast->users())
                    {
                        workList.push_back(pUser);
                    }
                }
                else if (GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pValue))
                {
                    // For GEPs we need to handle four cases:
                    // 1. The GEP is just pointing at the base object (unlikely but technically legal).
                    // 2. The GEP is pointing at the column of the matrix. In this case because we are handling a row
                    //    major matrix we need to turn the single GEP into a vector of GEPs, one for each element of the
                    //    the column (because the memory is not contiguous).
                    // 3. The GEP is getting a scalar element from a previously GEP'ed column, which means we are
                    //    actually just extracting an element from the vector of GEPs that we created above.
                    // 4. The GEP is pointing at a scalar element of the matrix.

                    assert(valueMap.count(pGetElemPtr->getPointerOperand()) > 0);

                    Value* const pRemappedValue = valueMap[pGetElemPtr->getPointerOperand()];

                    SmallVector<Value*, 8> indices;

                    for (Value* const pIndex : pGetElemPtr->indices())
                    {
                        indices.push_back(pIndex);
                    }

                    // Check that the first index is always zero.
                    assert(isa<ConstantInt>(indices[0]) && cast<ConstantInt>(indices[0])->isZero());

                    assert((indices.size() > 0) && (indices.size() < 4));

                    // If the GEP is just pointing at the base object, just update the value map.
                    if (indices.size() == 1)
                    {
                        valueMap[pGetElemPtr] = pRemappedValue;
                    }
                    else if (pRemappedValue->getType()->isPointerTy())
                    {
                        // If the value is a pointer type, we are indexing into the original matrix.
                        Value* const pRemappedValueSplat = getBuilder()->CreateVectorSplat(rowCount, pRemappedValue);
                        Value* pRowSplat = UndefValue::get(VectorType::get(getBuilder()->getInt32Ty(), rowCount));

                        for (uint32_t i = 0; i < rowCount; i++)
                        {
                            pRowSplat = getBuilder()->CreateInsertElement(pRowSplat, getBuilder()->getInt32(i), i);
                        }

                        Value* const pColumnSplat = getBuilder()->CreateVectorSplat(rowCount, indices[1]);

                        Value* const pNewGetElemPtr = getBuilder()->CreateGEP(pRemappedValueSplat,
                                                                         {
                                                                             getBuilder()->getInt32(0),
                                                                             pRowSplat,
                                                                             getBuilder()->getInt32(0),
                                                                             pColumnSplat
                                                                         });

                        // Check if we are loading a scalar element of the matrix or not.
                        if (indices.size() > 2)
                        {
                            valueMap[pGetElemPtr] = getBuilder()->CreateExtractElement(pNewGetElemPtr, indices[2]);
                        }
                        else
                        {
                            valueMap[pGetElemPtr] = pNewGetElemPtr;
                        }
                    }
                    else
                    {
                        // If we get here it means we are doing a subsequent GEP on a matrix row.
                        assert(pRemappedValue->getType()->isVectorTy());
                        assert(pRemappedValue->getType()->getVectorElementType()->isPointerTy());
                        valueMap[pGetElemPtr] = getBuilder()->CreateExtractElement(pRemappedValue, indices[1]);
                    }

                    // Add all the users of this GEP to the worklist for processing.
                    for (User* const pUser : pGetElemPtr->users())
                    {
                        workList.push_back(pUser);
                    }
                }
                else if (LoadInst* const pLoad = dyn_cast<LoadInst>(pValue))
                {
                    // For loads we have to handle three cases:
                    // 1. We are loading a full matrix, so do a load + transpose.
                    // 2. We are loading a column of a matrix, and since this is represented as a vector of GEPs we need
                    //    to issue a load for each element of this vector and recombine the result.
                    // 3. We are loading a single scalar element, do a simple load.

                    Value* const pPointer = valueMap[pLoad->getPointerOperand()];

                    // If the remapped pointer type isn't a pointer, it's a vector of pointers instead.
                    if (pPointer->getType()->isPointerTy() == false)
                    {
                        Type* const pPointerType = pPointer->getType();
                        assert(pPointerType->isVectorTy());

                        Value* pNewLoad = UndefValue::get(pLoad->getType());

                        for (uint32_t i = 0; i < pPointerType->getVectorNumElements(); i++)
                        {
                            Value* const pPointerElem = getBuilder()->CreateExtractElement(pPointer, i);

                            LoadInst* const pNewLoadElem = getBuilder()->CreateLoad(pPointerElem, pLoad->isVolatile());
                            pNewLoadElem->setOrdering(pLoad->getOrdering());
                            pNewLoadElem->setAlignment(MaybeAlign(pLoad->getAlignment()));
                            pNewLoadElem->setSyncScopeID(pLoad->getSyncScopeID());

                            if (pLoad->getMetadata(LLVMContext::MD_nontemporal))
                            {
                                transNonTemporalMetadata(pNewLoadElem);
                            }

                            pNewLoad = getBuilder()->CreateInsertElement(pNewLoad, pNewLoadElem, i);
                        }

                        pLoad->replaceAllUsesWith(pNewLoad);
                    }
                    else if (isTypeWithPadRowMajorMatrix(pPointer->getType()->getPointerElementType()))
                    {
                        Type* const pNewRowType = VectorType::get(pMatrixElementType, columnCount);
                        Type* const pNewLoadType = ArrayType::get(pNewRowType, rowCount);
                        Value* pNewLoad = UndefValue::get(pNewLoadType);

                        // If we are loading a full row major matrix, need to load the rows and then transpose.
                        for (uint32_t i = 0; i < rowCount; i++)
                        {
                            Value* pPointerElem = getBuilder()->CreateGEP(pPointer,
                                                                     {
                                                                        getBuilder()->getInt32(0),
                                                                        getBuilder()->getInt32(i),
                                                                        getBuilder()->getInt32(0)
                                                                     });
                            Type* pCastType = pPointerElem->getType()->getPointerElementType();
                            assert(pCastType->isArrayTy());
                            pCastType = VectorType::get(pCastType->getArrayElementType(),
                                                        pCastType->getArrayNumElements());
                            const uint32_t addrSpace = pPointerElem->getType()->getPointerAddressSpace();
                            pCastType = pCastType->getPointerTo(addrSpace);
                            pPointerElem = getBuilder()->CreateBitCast(pPointerElem, pCastType);

                            LoadInst* const pNewLoadElem = getBuilder()->CreateLoad(pPointerElem, pLoad->isVolatile());
                            pNewLoadElem->setOrdering(pLoad->getOrdering());
                            pNewLoadElem->setAlignment(MaybeAlign(pLoad->getAlignment()));
                            pNewLoadElem->setSyncScopeID(pLoad->getSyncScopeID());

                            if (pLoad->getMetadata(LLVMContext::MD_nontemporal))
                            {
                                transNonTemporalMetadata(pNewLoadElem);
                            }

                            pNewLoad = getBuilder()->CreateInsertValue(pNewLoad, pNewLoadElem, i);
                        }

                        pLoad->replaceAllUsesWith(getBuilder()->CreateTransposeMatrix(pNewLoad));
                    }
                    else
                    {
                        // Otherwise we are loading a single element and it's a simple load.
                        LoadInst* const pNewLoad = getBuilder()->CreateLoad(pPointer, pLoad->isVolatile());
                        pNewLoad->setOrdering(pLoad->getOrdering());
                        pNewLoad->setAlignment(MaybeAlign(pLoad->getAlignment()));
                        pNewLoad->setSyncScopeID(pLoad->getSyncScopeID());

                        if (pLoad->getMetadata(LLVMContext::MD_nontemporal))
                        {
                            transNonTemporalMetadata(pNewLoad);
                        }

                        pLoad->replaceAllUsesWith(pNewLoad);
                    }
                }
                else if (StoreInst* const pStore = dyn_cast<StoreInst>(pValue))
                {
                    // For stores we have to handle three cases:
                    // 1. We are storing a full matrix, so do a transpose + store.
                    // 2. We are storing a column of a matrix, and since this is represented as a vector of GEPs we need
                    //    to extract each element and issue a store.
                    // 3. We are storing a single scalar element, do a simple store.

                    Value* const pPointer = valueMap[pStore->getPointerOperand()];

                    // If the remapped pointer type isn't a pointer, it's a vector of pointers instead.
                    if (pPointer->getType()->isPointerTy() == false)
                    {
                        Type* const pPointerType = pPointer->getType();
                        assert(pPointerType->isVectorTy());

                        for (uint32_t i = 0; i < pPointerType->getVectorNumElements(); i++)
                        {
                            Value* pStoreValueElem = pStore->getValueOperand();

                            if (pStoreValueElem->getType()->isArrayTy())
                            {
                                pStoreValueElem = getBuilder()->CreateExtractValue(pStoreValueElem, i);
                            }
                            else
                            {
                                pStoreValueElem = getBuilder()->CreateExtractElement(pStoreValueElem, i);
                            }

                            Value* const pPointerElem = getBuilder()->CreateExtractElement(pPointer, i);

                            StoreInst* const pNewStoreElem = getBuilder()->CreateStore(pStoreValueElem,
                                                                                  pPointerElem,
                                                                                  pStore->isVolatile());
                            pNewStoreElem->setOrdering(pStore->getOrdering());
                            pNewStoreElem->setAlignment(MaybeAlign(pStore->getAlignment()));
                            pNewStoreElem->setSyncScopeID(pStore->getSyncScopeID());

                            if (pStore->getMetadata(LLVMContext::MD_nontemporal))
                            {
                                transNonTemporalMetadata(pNewStoreElem);
                            }
                        }
                    }
                    else if (isTypeWithPadRowMajorMatrix(pPointer->getType()->getPointerElementType()))
                    {
                        Value* pStoreValue = pStore->getValueOperand();

                        Type* const pStoreType = pStoreValue->getType();
                        Type* const pStoreElementType = pStoreType->getArrayElementType();
                        if (pStoreElementType->isArrayTy())
                        {
                            const uint32_t columnCount = pStoreType->getArrayNumElements();
                            const uint32_t rowCount = pStoreElementType->getArrayNumElements();

                            Type* const pColumnType = VectorType::get(pStoreElementType->getArrayElementType(),
                                                                      rowCount);
                            Type* const pMatrixType = ArrayType::get(pColumnType, columnCount);

                            Value* pMatrix = UndefValue::get(pMatrixType);

                            for (uint32_t column = 0, e = pStoreType->getArrayNumElements(); column < e; column++)
                            {
                                Value* pColumn = UndefValue::get(pColumnType);

                                for (uint32_t row = 0; row < rowCount; row++)
                                {
                                    Value* const pElement = getBuilder()->CreateExtractValue(pStoreValue, { column, row });
                                    pColumn = getBuilder()->CreateInsertElement(pColumn, pElement, row);
                                }

                                pMatrix = getBuilder()->CreateInsertValue(pMatrix, pColumn, column);
                            }

                            pStoreValue = pMatrix;
                        }

                        pStoreValue = getBuilder()->CreateTransposeMatrix(pStoreValue);

                        // If we are storing a full row major matrix, need to transpose then store the rows.
                        for (uint32_t i = 0; i < rowCount; i++)
                        {
                            Value* pPointerElem = getBuilder()->CreateGEP(pPointer,
                                                                     {
                                                                        getBuilder()->getInt32(0),
                                                                        getBuilder()->getInt32(i),
                                                                        getBuilder()->getInt32(0)
                                                                     });
                            Type* pCastType = pPointerElem->getType()->getPointerElementType();
                            assert(pCastType->isArrayTy());
                            pCastType = VectorType::get(pCastType->getArrayElementType(),
                                                        pCastType->getArrayNumElements());
                            const uint32_t addrSpace = pPointerElem->getType()->getPointerAddressSpace();
                            pCastType = pCastType->getPointerTo(addrSpace);
                            pPointerElem = getBuilder()->CreateBitCast(pPointerElem, pCastType);

                            Value* const pStoreValueElem = getBuilder()->CreateExtractValue(pStoreValue, i);

                            StoreInst* const pNewStoreElem = getBuilder()->CreateStore(pStoreValueElem,
                                                                                  pPointerElem,
                                                                                  pStore->isVolatile());
                            pNewStoreElem->setOrdering(pStore->getOrdering());
                            pNewStoreElem->setAlignment(MaybeAlign(pStore->getAlignment()));
                            pNewStoreElem->setSyncScopeID(pStore->getSyncScopeID());

                            if (pStore->getMetadata(LLVMContext::MD_nontemporal))
                            {
                                transNonTemporalMetadata(pNewStoreElem);
                            }
                        }
                    }
                    else
                    {
                        // Otherwise we are storing a single element and it's a simple store.
                        StoreInst* const pNewStore = getBuilder()->CreateStore(pStore->getValueOperand(),
                                                                          pPointer,
                                                                          pStore->isVolatile());
                        pNewStore->setOrdering(pStore->getOrdering());
                        pNewStore->setAlignment(MaybeAlign(pStore->getAlignment()));
                        pNewStore->setSyncScopeID(pStore->getSyncScopeID());

                        if (pStore->getMetadata(LLVMContext::MD_nontemporal))
                        {
                            transNonTemporalMetadata(pNewStore);
                        }
                    }
                }
                else
                {
                    llvm_unreachable("Should never be called!");
                }
            }
        }
    }

    const bool changed = (valuesToRemove.empty() == false);

    while (valuesToRemove.empty() == false)
    {
        Value* const pValue = valuesToRemove.pop_back_val();

        if (Instruction* const pInst = dyn_cast<Instruction>(pValue))
        {
            pInst->dropAllReferences();
            pInst->eraseFromParent();
        }
        else if (Function* const pFunc = dyn_cast<Function>(pValue))
        {
            pFunc->dropAllReferences();
            pFunc->eraseFromParent();
        }
        else
        {
            llvm_unreachable("Should never be called!");
        }
    }

    return changed;
}

/// Construct a DebugLoc for the given SPIRVInstruction.
DebugLoc SPIRVToLLVM::getDebugLoc(SPIRVInstruction *BI, Function *F) {
  if ((F == nullptr) || (!BI->hasLine()))
    return DebugLoc();
  auto Line = BI->getLine();
  return DebugLoc::get(
      Line->getLine(), Line->getColumn(),
      DbgTran.getDISubprogram(BI->getParent()->getParent(), F));
}

void SPIRVToLLVM::updateBuilderDebugLoc(SPIRVValue *BV, Function *F) {
  if (BV->isInst()) {
    SPIRVInstruction *BI = static_cast<SPIRVInstruction*>(BV);
    getBuilder()->SetCurrentDebugLocation(getDebugLoc(BI, F));
  }
}

// =====================================================================================================================
// Create a call to launder a row major matrix.
Value* SPIRVToLLVM::createLaunderRowMajorMatrix(
  Value* const pPointerToMatrix) // [in] The pointer to matrix to launder.
{
    Type* const pMatrixPointerType = pPointerToMatrix->getType();

    Type* const pMatrixType = pMatrixPointerType->getPointerElementType();
    assert(pMatrixType->isArrayTy() && pMatrixType->getArrayElementType()->isStructTy());

    Type* const pColumnVectorType = pMatrixType->getArrayElementType()->getStructElementType(0);
    assert(pColumnVectorType->isArrayTy());

    // Now we need to launder the row major matrix type into a column major one.
    Type* const pNewColumnVectorType = ArrayType::get(pColumnVectorType->getArrayElementType(),
                                                      pMatrixType->getArrayNumElements());
    Type* const pNewMatrixType = ArrayType::get(pNewColumnVectorType,
                                                pColumnVectorType->getArrayNumElements());
    Type* const pNewMatrixPointerType = pNewMatrixType->getPointerTo(
        pMatrixPointerType->getPointerAddressSpace());

    FunctionType* const pRowMajorFuncType = FunctionType::get(pNewMatrixPointerType,
                                                              pMatrixPointerType,
                                                              false);
    Function* const pRowMajorFunc = Function::Create(pRowMajorFuncType,
                                                     GlobalValue::ExternalLinkage,
                                                     SpirvLaunderRowMajor,
                                                     M);
    return getBuilder()->CreateCall(pRowMajorFunc, pPointerToMatrix);
}

// =====================================================================================================================
// Creates a load, taking care for types where we have had to add in explicit pads (structs with offset, arrays, and
// matrices) to only load the data that is being used. This will recursively step through the pointer to load from.
Value* SPIRVToLLVM::addLoadInstRecursively(
    SPIRVType* const pSpvType,      // [in] The SPIR-V type of the load.
    Value*           pLoadPointer,  // [in] The LLVM pointer to load from.
    bool             isVolatile,    // Is the load volatile?
    bool             isCoherent,    // Is the load coherent?
    bool             isNonTemporal) // Is the load non-temporal?
{
    assert(pLoadPointer->getType()->isPointerTy());

    Type* pLoadType = pLoadPointer->getType()->getPointerElementType();

    if (isTypeWithPadRowMajorMatrix(pLoadType))
    {
        pLoadPointer = createLaunderRowMajorMatrix(pLoadPointer);
        pLoadType = pLoadPointer->getType()->getPointerElementType();
    }

    Constant* const pZero = getBuilder()->getInt32(0);

    if (pLoadType->isStructTy() && (pSpvType->getOpCode() != OpTypeSampledImage) &&
        (pSpvType->getOpCode() != OpTypeImage))
    {
        // For structs we lookup the mapping of the elements and use it to reverse map the values.
        const bool needsPad = isRemappedTypeElements(pSpvType);

        SmallVector<Value*, 8> memberLoads;
        SmallVector<Type*, 8> memberTypes;

        for (uint32_t i = 0, memberCount = pSpvType->getStructMemberCount(); i < memberCount; i++)
        {
            const uint32_t memberIndex = needsPad ? lookupRemappedTypeElements(pSpvType, i) : i;

            Value* pMemberLoadPointer = getBuilder()->CreateGEP(pLoadPointer,
                                                           {
                                                                pZero,
                                                                getBuilder()->getInt32(memberIndex)
                                                           });

            // If the struct member was one which overlapped another member (as is common with HLSL cbuffer layout), we
            // need to handle the struct member carefully.
            auto pair = std::make_pair(pSpvType, i);
            if (OverlappingStructTypeWorkaroundMap.count(pair) > 0)
            {
                Type* const pType = OverlappingStructTypeWorkaroundMap[pair]->getPointerTo(
                    pMemberLoadPointer->getType()->getPointerAddressSpace());
                pMemberLoadPointer = getBuilder()->CreateBitCast(pMemberLoadPointer, pType);
            }

            Value* const pMemberLoad = addLoadInstRecursively(pSpvType->getStructMemberType(i),
                                                              pMemberLoadPointer,
                                                              isVolatile,
                                                              isCoherent,
                                                              isNonTemporal);

            memberLoads.push_back(pMemberLoad);
            memberTypes.push_back(pMemberLoad->getType());
        }

        Value* pLoad = UndefValue::get(StructType::get(M->getContext(), memberTypes));

        for (uint32_t i = 0, memberCount = pSpvType->getStructMemberCount(); i < memberCount; i++)
        {
            pLoad = getBuilder()->CreateInsertValue(pLoad, memberLoads[i], i);
        }

        return pLoad;
    }
    else if (pLoadType->isArrayTy() && (pSpvType->isTypeVector() == false))
    {
        // Matrix and arrays both get here. For both we need to turn [<{element-type, pad}>] into [element-type].
        const bool needsPad = isTypeWithPad(pLoadType);

        SPIRVType* const pSpvElementType = pSpvType->isTypeArray() ?
                                           pSpvType->getArrayElementType() :
                                           pSpvType->getMatrixColumnType();

        Type* pElementType = transType(pSpvElementType);

        Value* pLoad = UndefValue::get(ArrayType::get(pElementType, pLoadType->getArrayNumElements()));

        for (uint32_t i = 0, elementCount = pLoadType->getArrayNumElements(); i < elementCount; i++)
        {
            SmallVector<Value*, 3> indices;
            indices.push_back(pZero);
            indices.push_back(getBuilder()->getInt32(i));

            if (needsPad)
            {
                indices.push_back(pZero);
            }

            Value* pElementLoadPointer = getBuilder()->CreateGEP(pLoadPointer, indices);

            Value* const pElementLoad = addLoadInstRecursively(pSpvElementType,
                                                               pElementLoadPointer,
                                                               isVolatile,
                                                               isCoherent,
                                                               isNonTemporal);
            pLoad = getBuilder()->CreateInsertValue(pLoad, pElementLoad, i);
        }

        return pLoad;
    }
    else
    {
        Type* pAlignmentType = pLoadType;

        // Vectors are represented as arrays in memory, so we need to cast the array to a vector before loading.
        if (pSpvType->isTypeVector())
        {
            Type* const pVectorType = transType(pSpvType, 0, false, true, false);
            Type* const pCastType = pVectorType->getPointerTo(pLoadPointer->getType()->getPointerAddressSpace());
            pLoadPointer = getBuilder()->CreateBitCast(pLoadPointer, pCastType);

            const bool scalarBlockLayout = static_cast<Llpc::Context&>(getBuilder()->getContext()).
                                              GetScalarBlockLayout();

            if (scalarBlockLayout == false)
            {
                pAlignmentType = pVectorType;
            }
        }

        LoadInst* const pLoad = getBuilder()->CreateLoad(pLoadPointer, isVolatile);
        pLoad->setAlignment(MaybeAlign(M->getDataLayout().getABITypeAlignment(pAlignmentType)));

        if (isCoherent)
        {
            pLoad->setAtomic(AtomicOrdering::Unordered);
        }

        if (isNonTemporal)
        {
            transNonTemporalMetadata(pLoad);
        }

        // If the load was a bool or vector of bool, need to truncate the result.
        if (pSpvType->isTypeBool() ||
           (pSpvType->isTypeVector() && pSpvType->getVectorComponentType()->isTypeBool()))
        {
            return getBuilder()->CreateTruncOrBitCast(pLoad, transType(pSpvType));
        }
        else
        {
            return pLoad;
        }
    }
}

// =====================================================================================================================
// Creates a store, taking care for types where we have had to add in explicit pads (structs with offset, arrays, and
// matrices) to only store the data that is being used. This will recursively step through the value to store.
void SPIRVToLLVM::addStoreInstRecursively(
    SPIRVType* const pSpvType,      // [in] The SPIR-V type of the store.
    Value*           pStorePointer, // [in] The LLVM pointer to store to.
    Value*           pStoreValue,   // [in] The LLVM value to store into the pointer.
    bool             isVolatile,    // Is the store volatile?
    bool             isCoherent,    // Is the store coherent?
    bool             isNonTemporal) // Is the store non-temporal?
{
    assert(pStorePointer->getType()->isPointerTy());

    Type* pStoreType = pStorePointer->getType()->getPointerElementType();

    if (isTypeWithPadRowMajorMatrix(pStoreType))
    {
        pStorePointer = createLaunderRowMajorMatrix(pStorePointer);
        pStoreType = pStorePointer->getType()->getPointerElementType();
    }

    const uint32_t alignment = M->getDataLayout().getABITypeAlignment(pStoreType);

    // Special case if we are storing a constant value, we build up a modified constant, and store that - but only if
    // the alignment is greater than 1 (if the constant is storing an entire structure, because we have to use packed
    // structs to encoded layout information from SPIR-V into LLVM, we can very easily output large stores with align 1
    // that causes problems with the load/store vectorizer and DAG combining).
    if (isa<Constant>(pStoreValue) && (alignment > 1))
    {
        Constant* const pConstStoreValue = buildConstStoreRecursively(pSpvType,
                                                                      pStorePointer->getType(),
                                                                      cast<Constant>(pStoreValue));

        StoreInst* const pStore = getBuilder()->CreateStore(pConstStoreValue, pStorePointer, isVolatile);
        pStore->setAlignment(MaybeAlign(alignment));

        if (isCoherent)
        {
            pStore->setAtomic(AtomicOrdering::Unordered);
        }

        if (isNonTemporal)
        {
            transNonTemporalMetadata(pStore);
        }

        return;
    }

    Constant* const pZero = getBuilder()->getInt32(0);

    if (pStoreType->isStructTy() && (pSpvType->getOpCode() != OpTypeSampledImage) &&
        (pSpvType->getOpCode() != OpTypeImage))
    {
        // For structs we lookup the mapping of the elements and use it to map the values.
        const bool needsPad = isRemappedTypeElements(pSpvType);

        for (uint32_t i = 0, memberCount = pSpvType->getStructMemberCount(); i < memberCount; i++)
        {
            const uint32_t memberIndex = needsPad ? lookupRemappedTypeElements(pSpvType, i) : i;
            Value* const pMemberStorePointer = getBuilder()->CreateGEP(pStorePointer,
                                                                  {
                                                                      pZero,
                                                                      getBuilder()->getInt32(memberIndex)
                                                                  });
            Value* const pMemberStoreValue = getBuilder()->CreateExtractValue(pStoreValue, i);
            addStoreInstRecursively(pSpvType->getStructMemberType(i),
                                    pMemberStorePointer,
                                    pMemberStoreValue,
                                    isVolatile,
                                    isCoherent,
                                    isNonTemporal);
        }
    }
    else if (pStoreType->isArrayTy() && (pSpvType->isTypeVector() == false))
    {
        // Matrix and arrays both get here. For both we need to turn [element-type] into [<{element-type, pad}>].
        const bool needsPad = isTypeWithPad(pStoreType);

        SPIRVType* const pSpvElementType = pSpvType->isTypeArray() ?
                                           pSpvType->getArrayElementType() :
                                           pSpvType->getMatrixColumnType();

        for (uint32_t i = 0, elementCount = pStoreType->getArrayNumElements(); i < elementCount; i++)
        {
            SmallVector<Value*, 3> indices;
            indices.push_back(pZero);
            indices.push_back(getBuilder()->getInt32(i));

            if (needsPad)
            {
                indices.push_back(pZero);
            }

            Value* const pElementStorePointer = getBuilder()->CreateGEP(pStorePointer, indices);
            Value* const pElementStoreValue = getBuilder()->CreateExtractValue(pStoreValue, i);
            addStoreInstRecursively(pSpvElementType,
                                    pElementStorePointer,
                                    pElementStoreValue,
                                    isVolatile,
                                    isCoherent,
                                    isNonTemporal);
        }
    }
    else
    {
        Type* pAlignmentType = pStoreType;

        Type* pStoreType = nullptr;

        // If the store was a bool or vector of bool, need to zext the storing value.
        if (pSpvType->isTypeBool() ||
           (pSpvType->isTypeVector() && pSpvType->getVectorComponentType()->isTypeBool()))
        {
            pStoreValue = getBuilder()->CreateZExtOrBitCast(pStoreValue, pStorePointer->getType()->getPointerElementType());
            pStoreType = pStoreValue->getType();
        }
        else
        {
            pStoreType = transType(pSpvType);
        }

        // Vectors are represented as arrays in memory, so we need to cast the array to a vector before storing.
        if (pSpvType->isTypeVector())
        {
            Type* const pCastType = pStoreType->getPointerTo(pStorePointer->getType()->getPointerAddressSpace());
            pStorePointer = getBuilder()->CreateBitCast(pStorePointer, pCastType);

            const bool scalarBlockLayout = static_cast<Llpc::Context&>(getBuilder()->getContext()).
                                              GetScalarBlockLayout();

            if (scalarBlockLayout == false)
            {
                pAlignmentType = pStoreType;
            }
        }

        StoreInst* const pStore = getBuilder()->CreateStore(pStoreValue, pStorePointer, isVolatile);
        pStore->setAlignment(MaybeAlign(M->getDataLayout().getABITypeAlignment(pAlignmentType)));

        if (isCoherent)
        {
            pStore->setAtomic(AtomicOrdering::Unordered);
        }

        if (isNonTemporal)
        {
            transNonTemporalMetadata(pStore);
        }
    }
}

// =====================================================================================================================
// Build a modified constant to store.
Constant* SPIRVToLLVM::buildConstStoreRecursively(
    SPIRVType* const pSpvType,          // [in] The SPIR-V type of the store.
    Type* const      pStorePointerType, // [in] The LLVM pointer to store to.
    Constant*        pConstStoreValue)  // [in] The LLVM constant to store into the pointer.
{
    assert(pStorePointerType->isPointerTy());
    Type* const pStoreType = pStorePointerType->getPointerElementType();

    const uint32_t addrSpace = pStorePointerType->getPointerAddressSpace();

    Constant* const pZero = getBuilder()->getInt32(0);

    if (pStoreType->isStructTy() && (pSpvType->getOpCode() != OpTypeSampledImage) &&
        (pSpvType->getOpCode() != OpTypeImage))
    {
        // For structs we lookup the mapping of the elements and use it to map the values.
        const bool needsPad = isRemappedTypeElements(pSpvType);

        SmallVector<Constant*, 8> constMembers(pStoreType->getStructNumElements(), nullptr);

        // First run through the final LLVM type and create undef's for the members
        for (uint32_t i = 0, memberCount = constMembers.size(); i < memberCount; i++)
        {
            constMembers[i] = UndefValue::get(pStoreType->getStructElementType(i));
        }

        // Then run through the SPIR-V type and set the non-undef members to actual constants.
        for (uint32_t i = 0, memberCount = pSpvType->getStructMemberCount(); i < memberCount; i++)
        {
            const uint32_t memberIndex = needsPad ? lookupRemappedTypeElements(pSpvType, i) : i;
            Constant* indices[] = { pZero, getBuilder()->getInt32(memberIndex) };
            Type* const pMemberStoreType = GetElementPtrInst::getIndexedType(pStoreType, indices);
            constMembers[memberIndex] = buildConstStoreRecursively(pSpvType->getStructMemberType(i),
                                                                   pMemberStoreType->getPointerTo(addrSpace),
                                                                   pConstStoreValue->getAggregateElement(i));
        }

        return ConstantStruct::get(cast<StructType>(pStoreType), constMembers);
    }
    else if (pStoreType->isArrayTy() && (pSpvType->isTypeVector() == false))
    {
        // Matrix and arrays both get here. For both we need to turn [element-type] into [<{element-type, pad}>].
        const bool needsPad = isTypeWithPad(pStoreType);

        SmallVector<Constant*, 8> constElements(pStoreType->getArrayNumElements(),
                                                UndefValue::get(pStoreType->getArrayElementType()));

        SPIRVType* const pSpvElementType = pSpvType->isTypeArray() ?
                                           pSpvType->getArrayElementType() :
                                           pSpvType->getMatrixColumnType();

        for (uint32_t i = 0, elementCount = pStoreType->getArrayNumElements(); i < elementCount; i++)
        {
            SmallVector<Value*, 3> indices;
            indices.push_back(pZero);
            indices.push_back(getBuilder()->getInt32(i));

            if (needsPad)
            {
                indices.push_back(pZero);
            }

            Type* const pElementStoreType = GetElementPtrInst::getIndexedType(pStoreType, indices);
            Constant* const pConstElement = buildConstStoreRecursively(pSpvElementType,
                                                                       pElementStoreType->getPointerTo(addrSpace),
                                                                       pConstStoreValue->getAggregateElement(i));

            if (needsPad)
            {
                constElements[i] = ConstantExpr::getInsertValue(constElements[i], pConstElement, 0);
            }
            else
            {
                constElements[i] = pConstElement;
            }
        }

        return ConstantArray::get(cast<ArrayType>(pStoreType), constElements);
    }
    else
    {
        // If the store was a bool or vector of bool, need to zext the storing value.
        if (pSpvType->isTypeBool() ||
           (pSpvType->isTypeVector() && pSpvType->getVectorComponentType()->isTypeBool()))
        {
            pConstStoreValue = ConstantExpr::getZExtOrBitCast(pConstStoreValue, pStoreType);
        }

        // If the LLVM type is a not a vector, we need to change the constant into an array.
        if (pSpvType->isTypeVector() && (pStoreType->isVectorTy() == false))
        {
            assert(pStoreType->isArrayTy());

            SmallVector<Constant*, 8> constElements(pStoreType->getArrayNumElements(), nullptr);

            for (uint32_t i = 0, compCount = pSpvType->getVectorComponentCount(); i < compCount; i++)
            {
                constElements[i] = pConstStoreValue->getAggregateElement(i);
            }

            return ConstantArray::get(cast<ArrayType>(pStoreType), constElements);
        }

        return pConstStoreValue;
    }
}

// =====================================================================================================================
// Translate scope from SPIR-V to LLVM.
static SyncScope::ID transScope(
    LLVMContext&               context,   // [in] The LLVM context.
    const SPIRVConstant* const pSpvScope) // [in] The scope to translate.
{
    const uint32_t scope = static_cast<uint32_t>(pSpvScope->getZExtIntValue());

    switch (scope)
    {
    case ScopeCrossDevice:
    case ScopeDevice:
    case ScopeQueueFamilyKHR:
        return SyncScope::System;
    case ScopeInvocation:
        return SyncScope::SingleThread;
    case ScopeWorkgroup:
        return context.getOrInsertSyncScopeID("workgroup");
    case ScopeSubgroup:
        return context.getOrInsertSyncScopeID("wavefront");
    default:
        llvm_unreachable("Should never be called!");
        return SyncScope::System;
    }
}

// =====================================================================================================================
// Translate memory semantics from SPIR-V to LLVM.
static AtomicOrdering transMemorySemantics(
    const SPIRVConstant* const pSpvMemorySemantics, // [in] The semantics to translate.
    const bool                 isAtomicRMW)         // Is the memory semantic from an atomic rmw operation.
{
    const uint32_t semantics = static_cast<uint32_t>(pSpvMemorySemantics->getZExtIntValue());

    if (semantics & MemorySemanticsSequentiallyConsistentMask)
    {
        return AtomicOrdering::SequentiallyConsistent;
    }
    else if (semantics & MemorySemanticsAcquireReleaseMask)
    {
        return AtomicOrdering::AcquireRelease;
    }
    else if (semantics & MemorySemanticsAcquireMask)
    {
        return AtomicOrdering::Acquire;
    }
    else if (semantics & MemorySemanticsReleaseMask)
    {
        return AtomicOrdering::Release;
    }
    else if (semantics & (MemorySemanticsMakeAvailableKHRMask | MemorySemanticsMakeVisibleKHRMask))
    {
        return AtomicOrdering::Monotonic;
    }

    return AtomicOrdering::Monotonic;
}

// =====================================================================================================================
// Translate any read-modify-write atomics.
Value* SPIRVToLLVM::transAtomicRMW(
    SPIRVValue* const          pSpvValue, // [in] A SPIR-V value.
    const AtomicRMWInst::BinOp binOp)     // The binary operator.
{
    SPIRVAtomicInstBase* const pSpvAtomicInst = static_cast<SPIRVAtomicInstBase*>(pSpvValue);

    const SyncScope::ID scope = transScope(*Context, static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(1)));
    const AtomicOrdering ordering = transMemorySemantics(
        static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(2)), true);

    Value* const pAtomicPointer = transValue(pSpvAtomicInst->getOpValue(0),
                                             getBuilder()->GetInsertBlock()->getParent(),
                                             getBuilder()->GetInsertBlock());
    Value* const pAtomicValue = transValue(pSpvAtomicInst->getOpValue(3),
                                           getBuilder()->GetInsertBlock()->getParent(),
                                           getBuilder()->GetInsertBlock());

    return getBuilder()->CreateAtomicRMW(binOp, pAtomicPointer, pAtomicValue, ordering, scope);
}

// =====================================================================================================================
// Handle OpAtomicLoad.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicLoad>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    SPIRVAtomicLoad* const pSpvAtomicLoad = static_cast<SPIRVAtomicLoad*>(pSpvValue);

    const SyncScope::ID scope = transScope(*Context, static_cast<SPIRVConstant*>(pSpvAtomicLoad->getOpValue(1)));
    const AtomicOrdering ordering = transMemorySemantics(
        static_cast<SPIRVConstant*>(pSpvAtomicLoad->getOpValue(2)), false);

    Value* const pLoadPointer = transValue(pSpvAtomicLoad->getOpValue(0),
                                           getBuilder()->GetInsertBlock()->getParent(),
                                           getBuilder()->GetInsertBlock());

    LoadInst* const pLoad = getBuilder()->CreateLoad(pLoadPointer);

    const uint32_t loadAlignment = static_cast<uint32_t>(M->getDataLayout().getTypeSizeInBits(pLoad->getType()) / 8);
    pLoad->setAlignment(MaybeAlign(loadAlignment));
    pLoad->setAtomic(ordering, scope);

    return pLoad;
}

// =====================================================================================================================
// Handle OpAtomicStore.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicStore>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    SPIRVAtomicStore* const pSpvAtomicStore = static_cast<SPIRVAtomicStore*>(pSpvValue);

    const SyncScope::ID scope = transScope(*Context, static_cast<SPIRVConstant*>(pSpvAtomicStore->getOpValue(1)));
    const AtomicOrdering ordering = transMemorySemantics(
        static_cast<SPIRVConstant*>(pSpvAtomicStore->getOpValue(2)), false);

    Value* const pStorePointer = transValue(pSpvAtomicStore->getOpValue(0),
                                            getBuilder()->GetInsertBlock()->getParent(),
                                            getBuilder()->GetInsertBlock());
    Value* const pStoreValue = transValue(pSpvAtomicStore->getOpValue(3),
                                          getBuilder()->GetInsertBlock()->getParent(),
                                          getBuilder()->GetInsertBlock());

    StoreInst* const pStore = getBuilder()->CreateStore(pStoreValue, pStorePointer);

    const uint64_t storeSizeInBits = M->getDataLayout().getTypeSizeInBits(pStoreValue->getType());
    const uint32_t storeAlignment = static_cast<uint32_t>(storeSizeInBits / 8);
    pStore->setAlignment(MaybeAlign(storeAlignment));
    pStore->setAtomic(ordering, scope);

    return pStore;
}

// =====================================================================================================================
// Handle OpAtomicExchange.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicExchange>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::Xchg);
}

// =====================================================================================================================
// Handle OpAtomicIAdd.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicIAdd>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::Add);
}

// =====================================================================================================================
// Handle OpAtomicISub.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicISub>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::Sub);
}

// =====================================================================================================================
// Handle OpAtomicSMin.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicSMin>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::Min);
}

// =====================================================================================================================
// Handle OpAtomicUMin.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicUMin>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::UMin);
}

// =====================================================================================================================
// Handle OpAtomicSMax.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicSMax>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::Max);
}

// =====================================================================================================================
// Handle OpAtomicUMax.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicUMax>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::UMax);
}

// =====================================================================================================================
// Handle OpAtomicAnd.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicAnd>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::And);
}

// =====================================================================================================================
// Handle OpAtomicOr.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicOr>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::Or);
}

// =====================================================================================================================
// Handle OpAtomicXor.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicXor>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    return transAtomicRMW(pSpvValue, AtomicRMWInst::Xor);
}

// =====================================================================================================================
// Handle OpAtomicIIncrement.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicIIncrement>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    SPIRVAtomicInstBase* const pSpvAtomicInst = static_cast<SPIRVAtomicInstBase*>(pSpvValue);

    const SyncScope::ID scope = transScope(*Context, static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(1)));
    const AtomicOrdering ordering = transMemorySemantics(
        static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(2)), true);

    Value* const pAtomicPointer = transValue(pSpvAtomicInst->getOpValue(0),
                                             getBuilder()->GetInsertBlock()->getParent(),
                                             getBuilder()->GetInsertBlock());

    Value* const pOne = ConstantInt::get(pAtomicPointer->getType()->getPointerElementType(), 1);

    return getBuilder()->CreateAtomicRMW(AtomicRMWInst::Add, pAtomicPointer, pOne, ordering, scope);
}

// =====================================================================================================================
// Handle OpAtomicIDecrement.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicIDecrement>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    SPIRVAtomicInstBase* const pSpvAtomicInst = static_cast<SPIRVAtomicInstBase*>(pSpvValue);

    const SyncScope::ID scope = transScope(*Context, static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(1)));
    const AtomicOrdering ordering = transMemorySemantics(
        static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(2)), true);

    Value* const pAtomicPointer = transValue(pSpvAtomicInst->getOpValue(0),
                                             getBuilder()->GetInsertBlock()->getParent(),
                                             getBuilder()->GetInsertBlock());

    Value* const pOne = ConstantInt::get(pAtomicPointer->getType()->getPointerElementType(), 1);

    return getBuilder()->CreateAtomicRMW(AtomicRMWInst::Sub, pAtomicPointer, pOne, ordering, scope);
}

// =====================================================================================================================
// Handle OpAtomicCompareExchange.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAtomicCompareExchange>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    // Image texel atomic operations use the older path for now.
    if (static_cast<SPIRVInstruction*>(pSpvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer)
    {
        return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(pSpvValue),
                                               getBuilder()->GetInsertBlock());
    }

    SPIRVAtomicInstBase* const pSpvAtomicInst = static_cast<SPIRVAtomicInstBase*>(pSpvValue);

    const SyncScope::ID scope = transScope(*Context, static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(1)));
    const AtomicOrdering successOrdering = transMemorySemantics(
        static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(2)), true);
    const AtomicOrdering failureOrdering = transMemorySemantics(
        static_cast<SPIRVConstant*>(pSpvAtomicInst->getOpValue(3)), true);

    Value* const pAtomicPointer = transValue(pSpvAtomicInst->getOpValue(0),
                                             getBuilder()->GetInsertBlock()->getParent(),
                                             getBuilder()->GetInsertBlock());
    Value* const pExchangeValue = transValue(pSpvAtomicInst->getOpValue(4),
                                             getBuilder()->GetInsertBlock()->getParent(),
                                             getBuilder()->GetInsertBlock());
    Value* const pCompareValue = transValue(pSpvAtomicInst->getOpValue(5),
                                            getBuilder()->GetInsertBlock()->getParent(),
                                            getBuilder()->GetInsertBlock());

    AtomicCmpXchgInst* const pAtomicCmpXchg = getBuilder()->CreateAtomicCmpXchg(pAtomicPointer,
                                                                           pCompareValue,
                                                                           pExchangeValue,
                                                                           successOrdering,
                                                                           failureOrdering,
                                                                           scope);

    // LLVM cmpxchg returns { <ty>, i1 }, for SPIR-V we only care about the <ty>.
    return getBuilder()->CreateExtractValue(pAtomicCmpXchg, 0);
}

// =====================================================================================================================
// Handle OpCopyMemory.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpCopyMemory>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVCopyMemory* const pSpvCopyMemory = static_cast<SPIRVCopyMemory*>(pSpvValue);

    bool isSrcVolatile = pSpvCopyMemory->SPIRVMemoryAccess::isVolatile(true);

    // We don't require volatile on address spaces that become non-pointers.
    switch (pSpvCopyMemory->getSource()->getType()->getPointerStorageClass())
    {
    case StorageClassInput:
    case StorageClassOutput:
    case StorageClassPrivate:
    case StorageClassFunction:
        isSrcVolatile = false;
        break;
    default:
        break;
    }

    bool isDestVolatile = pSpvCopyMemory->SPIRVMemoryAccess::isVolatile(false);

    // We don't require volatile on address spaces that become non-pointers.
    switch (pSpvCopyMemory->getTarget()->getType()->getPointerStorageClass())
    {
    case StorageClassInput:
    case StorageClassOutput:
    case StorageClassPrivate:
    case StorageClassFunction:
        isDestVolatile = false;
        break;
    default:
        break;
    }

    bool isCoherent = false;

    if (pSpvCopyMemory->getMemoryAccessMask(true) & MemoryAccessMakePointerVisibleKHRMask)
    {
        SPIRVWord spvId = pSpvCopyMemory->getMakeVisibleScope(true);
        SPIRVConstant* const pSpvScope = static_cast<SPIRVConstant*>(BM->getValue(spvId));
        const uint32_t scope = pSpvScope->getZExtIntValue();

        const bool isSystemScope = ((scope <= ScopeDevice) || (scope == ScopeQueueFamilyKHR));

        if (isSystemScope)
        {
            isCoherent = true;
        }
    }

    if (pSpvCopyMemory->getMemoryAccessMask(false) & MemoryAccessMakePointerAvailableKHRMask)
    {
        SPIRVWord spvId = pSpvCopyMemory->getMakeAvailableScope(false);
        SPIRVConstant* const pSpvScope = static_cast<SPIRVConstant*>(BM->getValue(spvId));
        const uint32_t scope = pSpvScope->getZExtIntValue();

        const bool isSystemScope = ((scope <= ScopeDevice) || (scope == ScopeQueueFamilyKHR));

        if (isSystemScope)
        {
            isCoherent = true;
        }
    }

    bool isNonTemporal = pSpvCopyMemory->SPIRVMemoryAccess::isNonTemporal(true);

    Value* const pLoadPointer = transValue(pSpvCopyMemory->getSource(),
                                           getBuilder()->GetInsertBlock()->getParent(),
                                           getBuilder()->GetInsertBlock());

    SPIRVType* const pSpvLoadType = pSpvCopyMemory->getSource()->getType();

    Value* const pLoad = addLoadInstRecursively(pSpvLoadType->getPointerElementType(),
                                                pLoadPointer,
                                                isSrcVolatile,
                                                isCoherent,
                                                isNonTemporal);

    Value* const pStorePointer = transValue(pSpvCopyMemory->getTarget(),
                                            getBuilder()->GetInsertBlock()->getParent(),
                                            getBuilder()->GetInsertBlock());

    SPIRVType* const pSpvStoreType = pSpvCopyMemory->getTarget()->getType();
    isNonTemporal = pSpvCopyMemory->SPIRVMemoryAccess::isNonTemporal(false);

    addStoreInstRecursively(pSpvStoreType->getPointerElementType(),
                            pStorePointer,
                            pLoad,
                            isDestVolatile,
                            isCoherent,
                            isNonTemporal);
    return nullptr;
}

// =====================================================================================================================
// Handle OpLoad.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpLoad>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVLoad* const pSpvLoad = static_cast<SPIRVLoad*>(pSpvValue);

    // Handle UniformConstant image/sampler/sampledimage load.
    if (static_cast<SPIRVTypePointer*>(pSpvLoad->getSrc()->getType())->getStorageClass() == StorageClassUniformConstant)
    {
        switch (pSpvLoad->getType()->getOpCode())
        {
        case OpTypeImage:
        case OpTypeSampler:
        case OpTypeSampledImage:
            return transLoadImage(pSpvLoad->getSrc());
        default:
            break;
        }
    }

    bool isVolatile = pSpvLoad->SPIRVMemoryAccess::isVolatile(true);

    // We don't require volatile on address spaces that become non-pointers.
    switch (pSpvLoad->getSrc()->getType()->getPointerStorageClass())
    {
    case StorageClassInput:
    case StorageClassOutput:
    case StorageClassPrivate:
    case StorageClassFunction:
        isVolatile = false;
        break;
    default:
        break;
    }

    bool isCoherent = pSpvLoad->getSrc()->isCoherent();

    // MakePointerVisibleKHR is valid with OpLoad
    if (pSpvLoad->getMemoryAccessMask(true) & MemoryAccessMakePointerVisibleKHRMask)
    {
        SPIRVWord spvId = pSpvLoad->getMakeVisibleScope(true);
        SPIRVConstant* const pSpvScope = static_cast<SPIRVConstant*>(BM->getValue(spvId));
        const uint32_t scope = pSpvScope->getZExtIntValue();

        const bool isSystemScope = ((scope <= ScopeDevice) || (scope == ScopeQueueFamilyKHR));

        if (isSystemScope)
        {
            isCoherent = true;
        }
    }

    const bool isNonTemporal = pSpvLoad->SPIRVMemoryAccess::isNonTemporal(true);

    Value* const pLoadPointer = transValue(pSpvLoad->getSrc(),
                                           getBuilder()->GetInsertBlock()->getParent(),
                                           getBuilder()->GetInsertBlock());

    SPIRVType* const pSpvLoadType = pSpvLoad->getSrc()->getType();

    return addLoadInstRecursively(pSpvLoadType->getPointerElementType(),
                                  pLoadPointer,
                                  isVolatile,
                                  isCoherent,
                                  isNonTemporal);
}

// =====================================================================================================================
// Translate a load for UniformConstant that is image/sampler/sampledimage
Value* SPIRVToLLVM::transLoadImage(
    SPIRVValue* pSpvImageLoadPtr)  // [in] The image/sampler/sampledimage pointer
{
    SPIRVType* pSpvLoadedType = pSpvImageLoadPtr->getType()->getPointerElementType();
    Value* pBase = transImagePointer(pSpvImageLoadPtr);

    // Load the sampler.
    Value* pSampler = nullptr;
    auto typeOpcode = pSpvLoadedType->getOpCode();
    if (typeOpcode != OpTypeImage)
    {
        Value* pSamplerPtr = pBase;
        if (typeOpcode == OpTypeSampledImage)
        {
            pSamplerPtr = getBuilder()->CreateExtractValue(pBase, 1);
        }
        pSampler = getBuilder()->CreateLoadDescFromPtr(pSamplerPtr);
    }

    // Load the image.
    Value* pImage = nullptr;
    if (typeOpcode != OpTypeSampler)
    {
        bool multisampled = false;
        Value* pImagePtr = pBase;
        if (typeOpcode == OpTypeSampledImage)
        {
            pImagePtr = getBuilder()->CreateExtractValue(pBase, uint64_t(0));
            multisampled = static_cast<SPIRVTypeSampledImage*>(pSpvLoadedType)->getImageType()->getDescriptor().MS;
        }
        else
        {
            multisampled = static_cast<SPIRVTypeImage*>(pSpvLoadedType)->getDescriptor().MS;
        }

        if (multisampled)
        {
            // For a multisampled image, the pointer is in fact a struct containing a normal image descriptor
            // pointer and an fmask descriptor pointer.
            pImage = getBuilder()->CreateLoadDescFromPtr(getBuilder()->CreateExtractValue(pImagePtr, uint64_t(0)));
            Value* pFmask = getBuilder()->CreateLoadDescFromPtr(getBuilder()->CreateExtractValue(pImagePtr, 1));
            pImage = getBuilder()->CreateInsertValue(
                          UndefValue::get(StructType::get(*Context, { pImage->getType(), pFmask->getType() })),
                          pImage,
                          uint64_t(0));
            pImage = getBuilder()->CreateInsertValue(pImage, pFmask, 1);
        }
        else
        {
            pImage = getBuilder()->CreateLoadDescFromPtr(pImagePtr);
        }
    }

    // Return image or sampler, or join them together in a struct.
    if (pImage == nullptr)
    {
        return pSampler;
    }
    if (pSampler == nullptr)
    {
        return pImage;
    }

    Value* pResult = UndefValue::get(StructType::get(*Context, { pImage->getType(), pSampler->getType() }));
    pResult = getBuilder()->CreateInsertValue(pResult, pImage, uint64_t(0));
    pResult = getBuilder()->CreateInsertValue(pResult, pSampler, 1);
    return pResult;
}

// =====================================================================================================================
// Translate image/sampler/sampledimage pointer to IR value
Value* SPIRVToLLVM::transImagePointer(
    SPIRVValue* pSpvImagePtr)    // [in] The image/sampler/sampledimage pointer
{
    if ((pSpvImagePtr->getOpCode() != OpVariable) ||
        (static_cast<SPIRVTypePointer*>(pSpvImagePtr->getType())->getStorageClass() != StorageClassUniformConstant))
    {
        return transValue(pSpvImagePtr, getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
    }

    // For an image/sampler/sampledimage pointer that is a UniformConstant OpVariable, we need to materialize it by
    // generating the code to get the descriptor pointer(s).
    SPIRVWord descriptorSet = 0, binding = 0;
    pSpvImagePtr->hasDecorate(DecorationDescriptorSet, 0, &descriptorSet);
    pSpvImagePtr->hasDecorate(DecorationBinding, 0, &binding);

    Value* pDescPtr = nullptr;
    SPIRVType* pSpvImageTy = pSpvImagePtr->getType()->getPointerElementType();
    while ((pSpvImageTy->getOpCode() == OpTypeArray) || (pSpvImageTy->getOpCode() == OpTypeRuntimeArray))
    {
        pSpvImageTy = pSpvImageTy->getArrayElementType();
    }

    if (pSpvImageTy->getOpCode() == OpTypeSampler)
    {
        return getBuilder()->CreateGetSamplerDescPtr(descriptorSet, binding);
    }

    bool isSampledImage = (pSpvImageTy->getOpCode() == OpTypeSampledImage);
    if (isSampledImage)
    {
        pSpvImageTy = static_cast<SPIRVTypeSampledImage*>(pSpvImageTy)->getImageType();
    }
    assert(pSpvImageTy->getOpCode() == OpTypeImage);

    auto pDesc = &static_cast<SPIRVTypeImage*>(pSpvImageTy)->getDescriptor();
    if (pDesc->Dim == DimBuffer)
    {
        pDescPtr = getBuilder()->CreateGetTexelBufferDescPtr(descriptorSet, binding);
    }
    else
    {
        pDescPtr = getBuilder()->CreateGetImageDescPtr(descriptorSet, binding);
    }

    if (pDesc->MS)
    {
        // A multisampled image pointer is a struct containing an image desc pointer and an fmask desc pointer.
        Value* pFmaskDescPtr = getBuilder()->CreateGetFmaskDescPtr(descriptorSet, binding);
        pDescPtr = getBuilder()->CreateInsertValue(UndefValue::get(StructType::get(*Context,
                                                                                 {
                                                                                    pDescPtr->getType(),
                                                                                    pFmaskDescPtr->getType()
                                                                                 })),
                                                 pDescPtr,
                                                 uint64_t(0));
        pDescPtr = getBuilder()->CreateInsertValue(pDescPtr, pFmaskDescPtr, 1);
    }

    if (isSampledImage)
    {
        // A sampledimage pointer is a struct containing the image pointer and the sampler desc pointer.
        Value* pSamplerDescPtr = getBuilder()->CreateGetSamplerDescPtr(descriptorSet, binding);
        pDescPtr = getBuilder()->CreateInsertValue(UndefValue::get(StructType::get(*Context,
                                                                                 {
                                                                                    pDescPtr->getType(),
                                                                                    pSamplerDescPtr->getType()
                                                                                 })),
                                                 pDescPtr,
                                                 uint64_t(0));
        pDescPtr = getBuilder()->CreateInsertValue(pDescPtr, pSamplerDescPtr, 1);
    }

    return pDescPtr;
}

// =====================================================================================================================
// Handle OpStore.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpStore>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVStore* const pSpvStore = static_cast<SPIRVStore*>(pSpvValue);

    bool isVolatile = pSpvStore->SPIRVMemoryAccess::isVolatile(false);

    // We don't require volatile on address spaces that become non-pointers.
    switch (pSpvStore->getDst()->getType()->getPointerStorageClass())
    {
    case StorageClassInput:
    case StorageClassOutput:
    case StorageClassPrivate:
    case StorageClassFunction:
        isVolatile = false;
        break;
    default:
        break;
    }

    bool isCoherent = pSpvStore->getDst()->isCoherent();

    // MakePointerAvailableKHR is valid with OpStore
    if (pSpvStore->getMemoryAccessMask(false) & MemoryAccessMakePointerAvailableKHRMask)
    {
        SPIRVWord spvId = pSpvStore->getMakeAvailableScope(false);
        SPIRVConstant* const pSpvScope = static_cast<SPIRVConstant*>(BM->getValue(spvId));
        const uint32_t scope = pSpvScope->getZExtIntValue();

        const bool isSystemScope = ((scope <= ScopeDevice) || (scope == ScopeQueueFamilyKHR));

        if (isSystemScope)
        {
            isCoherent = true;
        }
    }

    const bool isNonTemporal = pSpvStore->SPIRVMemoryAccess::isNonTemporal(false);

    Value* const pStorePointer = transValue(pSpvStore->getDst(),
                                            getBuilder()->GetInsertBlock()->getParent(),
                                            getBuilder()->GetInsertBlock());

    Value* const pStoreValue = transValue(pSpvStore->getSrc(),
                                          getBuilder()->GetInsertBlock()->getParent(),
                                          getBuilder()->GetInsertBlock());

    SPIRVType* const pSpvStoreType = pSpvStore->getDst()->getType();

    addStoreInstRecursively(pSpvStoreType->getPointerElementType(),
                            pStorePointer,
                            pStoreValue,
                            isVolatile,
                            isCoherent,
                            isNonTemporal);

    // For stores, we don't really have a thing to map to, so we just return nullptr here.
    return nullptr;
}

// =====================================================================================================================
// Handle OpEndPrimitive
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpEndPrimitive>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return getBuilder()->CreateEndPrimitive(0);
}

// =====================================================================================================================
// Handle OpEndStreamPrimitive
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpEndStreamPrimitive>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    uint32_t streamId = static_cast<SPIRVConstant*>(static_cast<SPIRVInstTemplateBase*>(pSpvValue)->getOpValue(0))->
          getZExtIntValue();
    return getBuilder()->CreateEndPrimitive(streamId);
}

// =====================================================================================================================
// Handle OpArrayLength.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpArrayLength>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVArrayLength* const pSpvArrayLength = static_cast<SPIRVArrayLength*>(pSpvValue);
    SPIRVValue* const pSpvStruct = pSpvArrayLength->getStruct();
    assert(pSpvStruct->getType()->isTypePointer());

    Value* const pStruct = transValue(pSpvStruct, getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
    assert(pStruct->getType()->isPointerTy() && pStruct->getType()->getPointerElementType()->isStructTy());

    const uint32_t memberIndex = pSpvArrayLength->getMemberIndex();
    const uint32_t remappedMemberIndex = lookupRemappedTypeElements(pSpvStruct->getType()->getPointerElementType(),
                                                                    memberIndex);

    Value* const pBufferLength = getBuilder()->CreateGetBufferDescLength(pStruct);

    StructType* const pStructType = cast<StructType>(pStruct->getType()->getPointerElementType());
    const StructLayout* const pStructLayout = M->getDataLayout().getStructLayout(pStructType);
    const uint32_t offset = static_cast<uint32_t>(pStructLayout->getElementOffset(remappedMemberIndex));
    Value* const pOffset = getBuilder()->getInt32(offset);

    Type* const pMemberType = pStructType->getStructElementType(remappedMemberIndex)->getArrayElementType();
    const uint32_t stride = static_cast<uint32_t>(M->getDataLayout().getTypeSizeInBits(pMemberType) / 8);

    return getBuilder()->CreateUDiv(getBuilder()->CreateSub(pBufferLength, pOffset), getBuilder()->getInt32(stride));
}

// =====================================================================================================================
// Handle OpAccessChain.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpAccessChain>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVAccessChainBase* const pSpvAccessChain = static_cast<SPIRVAccessChainBase*>(pSpvValue);

    // Special handling for UniformConstant if the ultimate element type is image/sampler/sampledimage.
    if (static_cast<SPIRVTypePointer*>(pSpvAccessChain->getBase()->getType())->getStorageClass() ==
          StorageClassUniformConstant)
    {
        SPIRVType* pSpvUltimateElementType = pSpvAccessChain->getBase()->getType()->getPointerElementType();
        while ((pSpvUltimateElementType->getOpCode() == OpTypeArray) ||
               (pSpvUltimateElementType->getOpCode() == OpTypeRuntimeArray))
        {
            pSpvUltimateElementType = pSpvUltimateElementType->getArrayElementType();
        }

        switch (pSpvUltimateElementType->getOpCode())
        {
        case OpTypeImage:
        case OpTypeSampler:
        case OpTypeSampledImage:
            return transOpAccessChainForImage(pSpvAccessChain);
        default:
            break;
        }
    }

    // Non-image-related handling.
    Value* const pBase = transValue(pSpvAccessChain->getBase(),
                                    getBuilder()->GetInsertBlock()->getParent(),
                                    getBuilder()->GetInsertBlock());
    auto indices = transValue(pSpvAccessChain->getIndices(),
                              getBuilder()->GetInsertBlock()->getParent(),
                              getBuilder()->GetInsertBlock());

    truncConstantIndex(indices, getBuilder()->GetInsertBlock());

    if (pSpvAccessChain->hasPtrIndex() == false)
    {
        indices.insert(indices.begin(), getBuilder()->getInt32(0));
    }

    SPIRVType* const pSpvBaseType = pSpvAccessChain->getBase()->getType();
    Type* const pBasePointeeType = pBase->getType()->getPointerElementType();

    SPIRVType* pSpvAccessType = pSpvBaseType;

    // Records where (if at all) we have to split our indices - only required when going through a row_major matrix or
    // if we indexing into a struct that has partially overlapping offsets (normally occurs with HLSL cbuffer packing).
    SmallVector<std::pair<uint32_t, Type*>, 4> splits;

    const SPIRVStorageClassKind storageClass = pSpvBaseType->getPointerStorageClass();

    const bool isBufferBlockPointer = (storageClass == StorageClassStorageBuffer) ||
                                      (storageClass == StorageClassUniform) ||
                                      (storageClass == StorageClassPushConstant) ||
                                      (storageClass == StorageClassPhysicalStorageBufferEXT);

    // Run over the indices of the loop and investigate whether we need to add any additional indices so that we load
    // the correct data. We explicitly lay out our data in memory, which means because Vulkan has more powerful layout
    // options to producers than LLVM can model, we have had to insert manual padding into LLVM types to model this.
    // This loop will ensure that all padding is skipped in indexing.
    for (uint32_t i = 0; i < indices.size(); i++)
    {
        bool isDone = false;

        if (pSpvAccessType->isTypeForwardPointer())
        {
            pSpvAccessType = static_cast<SPIRVTypeForwardPointer*>(pSpvAccessType)->getPointer();
        }

        switch (pSpvAccessType->getOpCode())
        {
        case OpTypeStruct:
            {
                assert(isa<ConstantInt>(indices[i]));

                ConstantInt* const pConstIndex = cast<ConstantInt>(indices[i]);

                const uint64_t memberIndex = pConstIndex->getZExtValue();

                if (isBufferBlockPointer)
                {
                    if (isRemappedTypeElements(pSpvAccessType))
                    {
                        const uint64_t remappedMemberIndex = lookupRemappedTypeElements(pSpvAccessType, memberIndex);

                        // Replace the original index with the new remapped one.
                        indices[i] = getBuilder()->getInt32(remappedMemberIndex);
                    }

                    // If the struct member was actually overlapping another struct member, we need a split here.
                    const auto pair = std::make_pair(pSpvAccessType, memberIndex);

                    if (OverlappingStructTypeWorkaroundMap.count(pair) > 0)
                    {
                        splits.push_back(std::make_pair(i + 1, OverlappingStructTypeWorkaroundMap[pair]));
                    }
                }

                // Move the type we are looking at down into the member.
                pSpvAccessType = pSpvAccessType->getStructMemberType(memberIndex);
                break;
            }
        case OpTypeArray:
        case OpTypeRuntimeArray:
            {
                if (isBufferBlockPointer && isRemappedTypeElements(pSpvAccessType))
                {
                    // If we have padding in an array, we inserted a struct to add that
                    // padding, and so we need an extra constant 0 index.
                    indices.insert(indices.begin() + i + 1, getBuilder()->getInt32(0));

                    // Skip past the new idx we just added.
                    i++;
                }

                // Move the type we are looking at down into the element.
                pSpvAccessType = pSpvAccessType->getArrayElementType();
                break;
            }
        case OpTypeMatrix:
            {
                ArrayRef<Value*> sliceIndices(indices);
                sliceIndices = sliceIndices.take_front(i);

                Type* const pIndexedType = GetElementPtrInst::getIndexedType(pBasePointeeType, sliceIndices);

                // Matrices are represented as an array of columns.
                assert(pIndexedType && pIndexedType->isArrayTy());

                // If we have a row major matrix, we need to split the access chain here to handle it.
                if (isBufferBlockPointer && isTypeWithPadRowMajorMatrix(pIndexedType))
                {
                    splits.push_back(std::make_pair(i, nullptr));
                }
                else if (pIndexedType->getArrayElementType()->isStructTy())
                {
                    // If the type of the element is a struct we had to add padding to align, so need a further index.
                    indices.insert(indices.begin() + i + 1, getBuilder()->getInt32(0));

                    // Skip past the new idx we just added.
                    i++;
                }

                pSpvAccessType = pSpvAccessType->getMatrixColumnType();
                break;
            }
        case OpTypePointer:
            {
                pSpvAccessType = pSpvAccessType->getPointerElementType();
                break;
            }
        default:
            // We are either at the end of the index list, or we've hit a type that we definitely did not have to pad.
            {
                isDone = true;
                break;
            }
        }

        if (isDone)
        {
            break;
        }
    }

    if (isBufferBlockPointer)
    {
        Type* const pIndexedType = GetElementPtrInst::getIndexedType(pBasePointeeType, indices);

        // If we have a row major matrix, we need to split the access chain here to handle it.
        if (isTypeWithPadRowMajorMatrix(pIndexedType))
        {
            splits.push_back(std::make_pair(indices.size(), nullptr));
        }
    }

    if (splits.size() > 0)
    {
        Value* pNewBase = pBase;

        for (auto split : splits)
        {
            const ArrayRef<Value*> indexArray(indices);
            const ArrayRef<Value*> frontIndices(indexArray.take_front(split.first));

            // Get the pointer to our row major matrix first.
            if (pSpvAccessChain->isInBounds())
            {
                pNewBase = getBuilder()->CreateInBoundsGEP(pNewBase, frontIndices);
            }
            else
            {
                pNewBase = getBuilder()->CreateGEP(pNewBase, frontIndices);
            }

            // Matrix splits are identified by having a nullptr as the .second of the pair.
            if (split.second == nullptr)
            {
                pNewBase = createLaunderRowMajorMatrix(pNewBase);
            }
            else
            {
                Type* const pBitCastType = split.second->getPointerTo(pNewBase->getType()->getPointerAddressSpace());
                pNewBase = getBuilder()->CreateBitCast(pNewBase, pBitCastType);
            }

            // Lastly we remove the indices that we have already processed from the list of indices.
            uint32_t index = 0;

            // Always need at least a single index in back.
            indices[index++] = getBuilder()->getInt32(0);

            for (Value* const pIndex : indexArray.slice(split.first))
            {
                indices[index++] = pIndex;
            }

            indices.resize(index);
        }

        // Do the final index if we have one.
        if (pSpvAccessChain->isInBounds())
        {
            return getBuilder()->CreateInBoundsGEP(pNewBase, indices);
        }
        else
        {
            return getBuilder()->CreateGEP(pNewBase, indices);
        }
    }
    else
    {
        if (pSpvAccessChain->isInBounds())
        {
            return getBuilder()->CreateInBoundsGEP(pBase, indices);
        }
        else
        {
            return getBuilder()->CreateGEP(pBase, indices);
        }
    }
}

// =====================================================================================================================
// Handle OpAccessChain for pointer to (array of) image/sampler/sampledimage
Value* SPIRVToLLVM::transOpAccessChainForImage(
    SPIRVAccessChainBase* pSpvAccessChain)          // [in] The OpAccessChain
{
    SPIRVType* pSpvElementType = pSpvAccessChain->getBase()->getType()->getPointerElementType();
    std::vector<SPIRVValue*> spvIndicesVec = pSpvAccessChain->getIndices();
    ArrayRef<SPIRVValue*> spvIndices = spvIndicesVec;
    Value* pBase = transImagePointer(pSpvAccessChain->getBase());

    if (spvIndices.empty())
    {
        return pBase;
    }

    bool isNonUniform = spvIndices[0]->hasDecorate(DecorationNonUniformEXT);
    Value* pIndex = transValue(spvIndices[0], getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
    spvIndices = spvIndices.slice(1);
    pSpvElementType = pSpvElementType->getArrayElementType();

    while (pSpvElementType->getOpCode() == OpTypeArray)
    {
        pIndex = getBuilder()->CreateMul(pIndex,
                                       getBuilder()->getInt32(static_cast<SPIRVTypeArray*>(pSpvElementType)
                                                                ->getLength()->getZExtIntValue()));
        if (spvIndices.empty() == false)
        {
            isNonUniform |= spvIndices[0]->hasDecorate(DecorationNonUniformEXT);
            pIndex = getBuilder()->CreateAdd(pIndex,
                                           transValue(spvIndices[0],
                                                      getBuilder()->GetInsertBlock()->getParent(),
                                                      getBuilder()->GetInsertBlock()));
            spvIndices = spvIndices.slice(1);
        }
        pSpvElementType = pSpvElementType->getArrayElementType();
    }

    return indexDescPtr(pBase, pIndex, isNonUniform, pSpvElementType);
}

// =====================================================================================================================
// Apply an array index to a pointer to array of image/sampler/sampledimage.
// A pointer to sampledimage is in fact a structure containing pointer to image and pointer to sampler.
// A pointer to image when the image is multisampled is in fact a structure containing pointer to image
// and pointer to fmask descriptor.
Value* SPIRVToLLVM::indexDescPtr(
    Value*      pBase,              // [in] Base pointer to add index to
    Value*      pIndex,             // [in] Index value
    bool        isNonUniform,       // Whether the index is non-uniform
    SPIRVType*  pSpvElementType)    // Ultimate non-array element type (nullptr means assume single pointer)
{
    if (pSpvElementType != nullptr)
    {
        auto typeOpcode = pSpvElementType->getOpCode();
        if ((typeOpcode == OpTypeSampledImage) ||
            ((typeOpcode == OpTypeImage) && static_cast<SPIRVTypeImage*>(pSpvElementType)->getDescriptor().MS))
        {
            // These are the two cases that the pointer is in fact a structure containing two pointers.
            Value* pPtr0 = getBuilder()->CreateExtractValue(pBase, uint64_t(0));
            Value* pPtr1 = getBuilder()->CreateExtractValue(pBase, 1);
            SPIRVType* pSpvElementType0 = nullptr;
            if (typeOpcode == OpTypeSampledImage)
            {
                pSpvElementType0 = static_cast<SPIRVTypeSampledImage*>(pSpvElementType)->getImageType();
            }
            pPtr0 = indexDescPtr(pPtr0,
                                 pIndex,
                                 isNonUniform,
                                 pSpvElementType0);
            pPtr1 = indexDescPtr(pPtr1,
                                 pIndex,
                                 isNonUniform,
                                 nullptr);
            pBase = getBuilder()->CreateInsertValue(UndefValue::get(pBase->getType()), pPtr0, uint64_t(0));
            pBase = getBuilder()->CreateInsertValue(pBase, pPtr1, 1);
            return pBase;
        }
    }

    return getBuilder()->CreateIndexDescPtr(pBase, pIndex, isNonUniform);
}

// =====================================================================================================================
// Handle OpInBoundsAccessChain.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpInBoundsAccessChain>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transValueWithOpcode<OpAccessChain>(pSpvValue);
}

// =====================================================================================================================
// Handle OpPtrAccessChain.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpPtrAccessChain>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transValueWithOpcode<OpAccessChain>(pSpvValue);
}

// =====================================================================================================================
// Handle OpInBoundsPtrAccessChain.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpInBoundsPtrAccessChain>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transValueWithOpcode<OpAccessChain>(pSpvValue);
}

// =====================================================================================================================
// Handle OpImage (extract image from sampledimage)
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpImage>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    Value* pSampledImage = transValue(static_cast<SPIRVInstTemplateBase*>(pSpvValue)->getOpValue(0),
                                      getBuilder()->GetInsertBlock()->getParent(),
                                      getBuilder()->GetInsertBlock());
    return getBuilder()->CreateExtractValue(pSampledImage, uint64_t(0));
}

// =====================================================================================================================
// Handle OpSampledImage (combine image and sampler to create sampledimage)
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpSampledImage>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    Value* pImage = transValue(static_cast<SPIRVInstTemplateBase*>(pSpvValue)->getOpValue(0),
                               getBuilder()->GetInsertBlock()->getParent(),
                               getBuilder()->GetInsertBlock());
    Value* pSampler = transValue(static_cast<SPIRVInstTemplateBase*>(pSpvValue)->getOpValue(1),
                                 getBuilder()->GetInsertBlock()->getParent(),
                                 getBuilder()->GetInsertBlock());

    Value* pResult = UndefValue::get(StructType::get(*Context, { pImage->getType(), pSampler->getType() }));
    pResult = getBuilder()->CreateInsertValue(pResult, pImage, uint64_t(0));
    pResult = getBuilder()->CreateInsertValue(pResult, pSampler, 1);
    return pResult;
}

// =====================================================================================================================
// Handle OpKill.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpKill>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    Value* const pKill = getBuilder()->CreateKill();

    // NOTE: In SPIR-V, "OpKill" is considered as a valid instruction to terminate blocks. But in LLVM, we have to
    // insert a dummy "return" instruction as block terminator.
    if (getBuilder()->getCurrentFunctionReturnType()->isVoidTy())
    {
        // No return value
        getBuilder()->CreateRetVoid();
    }
    else
    {
        // Function returns value
        getBuilder()->CreateRet(UndefValue::get(getBuilder()->getCurrentFunctionReturnType()));
    }

    return pKill;
}

// =====================================================================================================================
// Handle OpDemoteToHelperInvocationEXT.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpDemoteToHelperInvocationEXT>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return getBuilder()->CreateDemoteToHelperInvocation();
}

// =====================================================================================================================
// Handle OpIsHelperInvocationEXT.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpIsHelperInvocationEXT>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return getBuilder()->CreateIsHelperInvocation();
}

// =====================================================================================================================
// Handle OpReadClockKHR.
template<> Value* SPIRVToLLVM::transValueWithOpcode<spv::OpReadClockKHR>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    SPIRVConstant* const pSpvScope = static_cast<SPIRVConstant*>(pSpvInst->getOperands()[0]);
    const spv::Scope scope = static_cast<spv::Scope>(pSpvScope->getZExtIntValue());
    assert((scope == spv::ScopeDevice) || (scope == spv::ScopeWorkgroup));

    Value* const pReadClock = getBuilder()->CreateReadClock(scope == spv::ScopeDevice);

    SPIRVType* const pSpvType = pSpvInst->getType();
    if (pSpvType->isTypeVectorInt(32))
    {
        assert(pSpvType->getVectorComponentCount() == 2); // Must be uvec2
        return getBuilder()->CreateBitCast(pReadClock, transType(pSpvType)); // uint64 -> uvec2
    }
    else
    {
        assert(pSpvType->isTypeInt(64));
        return pReadClock;
    }
}

// =====================================================================================================================
// Handle OpGroupAll.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupAll>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pPredicate = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupAll(pPredicate);
}

// =====================================================================================================================
// Handle OpGroupAny.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupAny>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pPredicate = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupAny(pPredicate);
}

// =====================================================================================================================
// Handle OpGroupBroadcast.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupBroadcast>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    Value* const pId = transValue(spvOperands[2], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBroadcast(pValue, pId);
}

// =====================================================================================================================
// Handle OpGroupIAdd.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupIAdd>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::IAdd, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupFAdd.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupFAdd>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FAdd, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupSMin.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupSMin>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::SMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupUMin.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupUMin>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::UMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupFMin.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupFMin>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupSMax.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupSMax>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::SMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupUMax.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupUMax>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::UMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupFMax.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupFMax>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformElect.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformElect>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return getBuilder()->CreateSubgroupElect();
}

// =====================================================================================================================
// Handle OpGroupNonUniformAll.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformAll>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pPredicate = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupAll(pPredicate, ModuleUsage->useHelpInvocation);
}

// =====================================================================================================================
// Handle OpGroupNonUniformAny.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformAny>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pPredicate = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupAny(pPredicate, ModuleUsage->useHelpInvocation);
}

// =====================================================================================================================
// Handle OpGroupNonUniformAllEqual.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformAllEqual>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupAllEqual(pValue, ModuleUsage->useHelpInvocation);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBroadcast.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBroadcast>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    Value* const pIndex = transValue(spvOperands[2], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBroadcast(pValue, pIndex);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBroadcastFirst.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBroadcastFirst>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBroadcastFirst(pValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallot.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallot>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pPredicate = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBallot(pPredicate);
}

// =====================================================================================================================
// Handle OpGroupNonUniformInverseBallot.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformInverseBallot>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupInverseBallot(pValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallotBitExtract.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallotBitExtract>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    Value* const pIndex = transValue(spvOperands[2], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBallotBitExtract(pValue, pIndex);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallotBitCount.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallotBitCount>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[2], pFunc, pBlock);

    switch (static_cast<SPIRVConstant*>(spvOperands[1])->getZExtIntValue())
    {
    case GroupOperationReduce:
        return getBuilder()->CreateSubgroupBallotBitCount(pValue);
    case GroupOperationInclusiveScan:
        return getBuilder()->CreateSubgroupBallotInclusiveBitCount(pValue);
    case GroupOperationExclusiveScan:
        return getBuilder()->CreateSubgroupBallotExclusiveBitCount(pValue);
    default:
        llvm_unreachable("Should never be called!");
        return nullptr;
    }
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallotFindLSB.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallotFindLSB>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBallotFindLsb(pValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallotFindMSB.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallotFindMSB>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBallotFindMsb(pValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformShuffle.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformShuffle>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    Value* const pIndex = transValue(spvOperands[2], pFunc, pBlock);
    return getBuilder()->CreateSubgroupShuffle(pValue, pIndex);
}

// =====================================================================================================================
// Handle OpGroupNonUniformShuffleXor.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformShuffleXor>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    Value* const pMask = transValue(spvOperands[2], pFunc, pBlock);
    return getBuilder()->CreateSubgroupShuffleXor(pValue, pMask);
}

// =====================================================================================================================
// Handle OpGroupNonUniformShuffleUp.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformShuffleUp>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    Value* const pDelta = transValue(spvOperands[2], pFunc, pBlock);
    return getBuilder()->CreateSubgroupShuffleUp(pValue, pDelta);
}

// =====================================================================================================================
// Handle OpGroupNonUniformShuffleDown.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformShuffleDown>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    Value* const pDelta = transValue(spvOperands[2], pFunc, pBlock);
    return getBuilder()->CreateSubgroupShuffleDown(pValue, pDelta);
}

// =====================================================================================================================
// Handle a group arithmetic operation.
Value* SPIRVToLLVM::transGroupArithOp(
    Builder::GroupArithOp groupArithOp, // The group operation.
    SPIRVValue* const           pSpvValue)  // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();

    Value* const pValue = transValue(spvOperands[2], pFunc, pBlock);

    switch (static_cast<SPIRVConstant*>(spvOperands[1])->getZExtIntValue())
    {
    case GroupOperationReduce:
        return getBuilder()->CreateSubgroupClusteredReduction(groupArithOp, pValue, getBuilder()->CreateGetSubgroupSize());
    case GroupOperationInclusiveScan:
        return getBuilder()->CreateSubgroupClusteredInclusive(groupArithOp, pValue, getBuilder()->CreateGetSubgroupSize());
    case GroupOperationExclusiveScan:
        return getBuilder()->CreateSubgroupClusteredExclusive(groupArithOp, pValue, getBuilder()->CreateGetSubgroupSize());
    case GroupOperationClusteredReduce:
        return getBuilder()->CreateSubgroupClusteredReduction(groupArithOp,
                                                         pValue,
                                                         transValue(spvOperands[3], pFunc, pBlock));
    default:
        llvm_unreachable("Should never be called!");
        return nullptr;
    }
}

// =====================================================================================================================
// Handle OpGroupNonUniformIAdd.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformIAdd>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::IAdd, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformFAdd.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformFAdd>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FAdd, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformIMul.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformIMul>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::IMul, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformFMul.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformFMul>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FMul, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformSMin.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformSMin>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::SMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformUMin.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformUMin>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::UMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformFMin.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformFMin>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformSMax.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformSMax>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::SMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformUMax.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformUMax>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::UMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformFMax.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformFMax>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBitwiseAnd.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBitwiseAnd>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::And, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBitwiseOr.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBitwiseOr>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::Or, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBitwiseXor.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBitwiseXor>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::Xor, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformLogicalAnd.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformLogicalAnd>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::And, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformLogicalOr.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformLogicalOr>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::Or, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformLogicalXor.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformLogicalXor>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::Xor, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformQuadBroadcast.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformQuadBroadcast>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);
    Value* const pIndex = transValue(spvOperands[2], pFunc, pBlock);
    return getBuilder()->CreateSubgroupQuadBroadcast(pValue, pIndex);
}

// =====================================================================================================================
// Handle OpGroupNonUniformQuadSwap.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformQuadSwap>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    assert(static_cast<SPIRVConstant*>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[1], pFunc, pBlock);

    switch (static_cast<SPIRVConstant*>(spvOperands[2])->getZExtIntValue())
    {
    case 0:
        return getBuilder()->CreateSubgroupQuadSwapHorizontal(pValue);
    case 1:
        return getBuilder()->CreateSubgroupQuadSwapVertical(pValue);
    case 2:
        return getBuilder()->CreateSubgroupQuadSwapDiagonal(pValue);
    default:
        llvm_unreachable("Should never be called!");
        return nullptr;
    }
}

// =====================================================================================================================
// Handle OpSubgroupBallotKHR.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpSubgroupBallotKHR>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pPredicate = transValue(spvOperands[0], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBallot(pPredicate);
}

// =====================================================================================================================
// Handle OpSubgroupFirstInvocationKHR.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpSubgroupFirstInvocationKHR>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[0], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBroadcastFirst(pValue);
}

// =====================================================================================================================
// Handle OpSubgroupAllKHR.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpSubgroupAllKHR>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pPredicate = transValue(spvOperands[0], pFunc, pBlock);
    return getBuilder()->CreateSubgroupAll(pPredicate, ModuleUsage->useHelpInvocation);
}

// =====================================================================================================================
// Handle OpSubgroupAnyKHR.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpSubgroupAnyKHR>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pPredicate = transValue(spvOperands[0], pFunc, pBlock);
    return getBuilder()->CreateSubgroupAny(pPredicate, ModuleUsage->useHelpInvocation);
}

// =====================================================================================================================
// Handle OpSubgroupAllEqualKHR.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpSubgroupAllEqualKHR>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[0], pFunc, pBlock);
    return getBuilder()->CreateSubgroupAllEqual(pValue, ModuleUsage->useHelpInvocation);
}

// =====================================================================================================================
// Handle OpSubgroupReadInvocationKHR.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpSubgroupReadInvocationKHR>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pValue = transValue(spvOperands[0], pFunc, pBlock);
    Value* const pIndex = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateSubgroupBroadcast(pValue, pIndex);
}

// =====================================================================================================================
// Handle OpGroupIAddNonUniformAMD.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupIAddNonUniformAMD>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::IAdd, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupFAddNonUniformAMD.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupFAddNonUniformAMD>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FAdd, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupSMinNonUniformAMD.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupSMinNonUniformAMD>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::SMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupUMinNonUniformAMD.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupUMinNonUniformAMD>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::UMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupFMinNonUniformAMD.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupFMinNonUniformAMD>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FMin, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupSMaxNonUniformAMD.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupSMaxNonUniformAMD>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::SMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupUMaxNonUniformAMD.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupUMaxNonUniformAMD>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::UMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpGroupFMaxNonUniformAMD.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpGroupFMaxNonUniformAMD>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    return transGroupArithOp(Builder::GroupArithOp::FMax, pSpvValue);
}

// =====================================================================================================================
// Handle OpExtInst.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpExtInst>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVExtInst* const pSpvExtInst = static_cast<SPIRVExtInst*>(pSpvValue);

    // Just ignore this set of extended instructions
    if (BM->getBuiltinSet(pSpvExtInst->getExtSetId()) == SPIRVEIS_NonSemanticInfo)
        return nullptr;

    std::vector<SPIRVValue*> spvArgValues = pSpvExtInst->getArgumentValues();

    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = pBlock->getParent();

    switch (BM->getBuiltinSet(pSpvExtInst->getExtSetId()))
    {
    case SPIRVEIS_ShaderBallotAMD:
        switch (pSpvExtInst->getExtOp())
        {
        case SwizzleInvocationsAMD:
            return getBuilder()->CreateSubgroupSwizzleQuad(transValue(spvArgValues[0], pFunc, pBlock),
                                                      transValue(spvArgValues[1], pFunc, pBlock));
        case SwizzleInvocationsMaskedAMD:
            return getBuilder()->CreateSubgroupSwizzleMask(transValue(spvArgValues[0], pFunc, pBlock),
                                                      transValue(spvArgValues[1], pFunc, pBlock));
        case WriteInvocationAMD:
            return getBuilder()->CreateSubgroupWriteInvocation(transValue(spvArgValues[0], pFunc, pBlock),
                                                          transValue(spvArgValues[1], pFunc, pBlock),
                                                          transValue(spvArgValues[2], pFunc, pBlock));
        case MbcntAMD:
            return getBuilder()->CreateSubgroupMbcnt(transValue(spvArgValues[0], pFunc, pBlock));
        default:
            llvm_unreachable("Should never be called!");
            return nullptr;
        }
    case SPIRVEIS_GLSL:
        return transGLSLExtInst(pSpvExtInst, pBlock);

    case SPIRVEIS_ShaderExplicitVertexParameterAMD:
        return transGLSLBuiltinFromExtInst(pSpvExtInst, pBlock);

    case SPIRVEIS_GcnShaderAMD:
        switch (pSpvExtInst->getExtOp())
        {
        case CubeFaceCoordAMD:
            return getBuilder()->CreateCubeFaceCoord(transValue(spvArgValues[0], pFunc, pBlock));
        case CubeFaceIndexAMD:
            return getBuilder()->CreateCubeFaceIndex(transValue(spvArgValues[0], pFunc, pBlock));
        case TimeAMD:
            return getBuilder()->CreateReadClock(false);
        default:
            llvm_unreachable("Should never be called!");
            return nullptr;
        }

    case SPIRVEIS_ShaderTrinaryMinMaxAMD:
        return transTrinaryMinMaxExtInst(pSpvExtInst, pBlock);

    default:
        llvm_unreachable("Should never be called!");
        return nullptr;
    }
}

// =====================================================================================================================
// Translate an initializer. This has special handling for the case where the type to initialize to does not match the
// type of the initializer, which is common when dealing with interface objects.
Constant* SPIRVToLLVM::transInitializer(
    SPIRVValue* const pSpvValue, // [in] The SPIR-V value that is an initializer.
    Type* const       pType)     // [in] The LLVM type of the initializer.
{
    SPIRVType* const pSpvType = pSpvValue->getType();

    if ((pSpvValue->getOpCode() == OpConstantNull) && pType->isAggregateType())
    {
        return ConstantAggregateZero::get(pType);
    }

    if (pSpvType->isTypeStruct())
    {
        SPIRVConstantComposite* const pSpvConstStruct = static_cast<SPIRVConstantComposite*>(pSpvValue);

        std::vector<SPIRVValue *> spvMembers(pSpvConstStruct->getElements());
        assert(spvMembers.size() == pSpvType->getStructMemberCount());

        // For structs we lookup the mapping of the elements and use it to reverse map the values.
        const bool needsPad = isRemappedTypeElements(pSpvType);

        assert((needsPad == false) || isRemappedTypeElements(pSpvType));

        Constant* pStructInitializer = UndefValue::get(pType);

        for (uint32_t i = 0, memberCount = spvMembers.size(); i < memberCount; i++)
        {
            const uint32_t memberIndex = needsPad ? lookupRemappedTypeElements(pSpvType, i) : i;

            Constant* const pInitializer = transInitializer(spvMembers[i], pType->getStructElementType(memberIndex));

            pStructInitializer = ConstantExpr::getInsertValue(pStructInitializer, pInitializer, memberIndex);
        }

        return pStructInitializer;
    }
    else if (pType->isArrayTy())
    {
        SPIRVConstantComposite* const pSpvConstArray = static_cast<SPIRVConstantComposite*>(pSpvValue);

        std::vector<SPIRVValue *> spvElements(pSpvConstArray->getElements());
        assert(spvElements.size() == pType->getArrayNumElements());

        // Matrix and arrays both get here. For both we need to turn [<{element-type, pad}>] into [element-type].
        const bool needsPad = isTypeWithPad(pType);

        Constant* pArrayInitializer = UndefValue::get(pType);

        for (uint32_t i = 0, elementCount = spvElements.size(); i < elementCount; i++)
        {
            if (needsPad)
            {
                Type* const pElementType = pType->getArrayElementType()->getStructElementType(0);
                Constant* const pInitializer = transInitializer(spvElements[i], pElementType);
                pArrayInitializer = ConstantExpr::getInsertValue(pArrayInitializer, pInitializer, { i, 0 });
            }
            else
            {
                Type* const pElementType = pType->getArrayElementType();
                Constant* const pInitializer = transInitializer(spvElements[i], pElementType);
                pArrayInitializer = ConstantExpr::getInsertValue(pArrayInitializer, pInitializer, i);
            }
        }

        return pArrayInitializer;
    }
    else
    {
        Constant* pInitializer = cast<Constant>(transValue(pSpvValue, nullptr, nullptr, false));
        if (pInitializer->getType() != pType)
        {
            // The translated value type is different to the requested type. This can only happen in the
            // case that the SPIR-V value was bool but the requested type was i32 because it is a bool
            // in memory.
            assert(pInitializer->getType()->isIntegerTy(1));
            assert(pType->isIntegerTy(32));
            pInitializer = ConstantExpr::getZExt(pInitializer, pType);
        }
        return pInitializer;
    }
}

// =====================================================================================================================
// Handle OpVariable.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpVariable>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVVariable* const pSpvVar = static_cast<SPIRVVariable*>(pSpvValue);
    const SPIRVStorageClassKind storageClass = pSpvVar->getStorageClass();
    SPIRVType* const pSpvVarType = pSpvVar->getType()->getPointerElementType();

    if (storageClass == StorageClassUniformConstant)
    {
        SPIRVType* pSpvElementType = pSpvVarType;
        while ((pSpvElementType->getOpCode() == OpTypeArray) || (pSpvElementType->getOpCode() == OpTypeRuntimeArray))
        {
            pSpvElementType = pSpvElementType->getArrayElementType();
        }
        switch (pSpvElementType->getOpCode())
        {
        case OpTypeImage:
        case OpTypeSampler:
        case OpTypeSampledImage:
            // Do nothing for image/sampler/sampledimage.
            return nullptr;
        default:
            break;
        }
    }

    Type* const pPtrType = transType(pSpvVar->getType());
    Type* const pVarType = pPtrType->getPointerElementType();

    SPIRVValue* const pSpvInitializer = pSpvVar->getInitializer();

    Constant* pInitializer = nullptr;

    // If the type has an initializer, re-create the SPIR-V initializer in LLVM.
    if (pSpvInitializer != nullptr)
    {
        pInitializer = transInitializer(pSpvInitializer, pVarType);
    }
    else if (storageClass == SPIRVStorageClassKind::StorageClassWorkgroup)
    {
        pInitializer = UndefValue::get(pVarType);
    }

    if (storageClass == StorageClassFunction)
    {
        assert(getBuilder()->GetInsertBlock() != nullptr);

        Value* const pVar = getBuilder()->CreateAlloca(pVarType, nullptr, pSpvVar->getName());

        if (pInitializer != nullptr)
        {
            getBuilder()->CreateStore(pInitializer, pVar);
        }

        return pVar;
    }

    bool readOnly = false;

    switch (storageClass)
    {
    case StorageClassPushConstant:
        {
            readOnly = true;
            break;
        }
    case StorageClassStorageBuffer:
    case StorageClassUniform:
        {
            SPIRVType* pSpvBlockDecoratedType = pSpvVarType;

            // Skip through arrays of descriptors to get to the descriptor block type.
            while (pSpvBlockDecoratedType->isTypeArray())
            {
                pSpvBlockDecoratedType = pSpvBlockDecoratedType->getArrayElementType();
            }

            assert(pSpvBlockDecoratedType->isTypeStruct());

            readOnly = pSpvBlockDecoratedType->hasDecorate(DecorationBlock) &&
                       (storageClass != SPIRVStorageClassKind::StorageClassStorageBuffer);
            break;
        }
    default:
        {
            break;
        }
    }

    if (pSpvVar->hasDecorate(DecorationNonWritable))
    {
        readOnly = true;
    }
    else if (pSpvVarType->isTypeStruct())
    {
        // glslang has a bug where it'll output NonWritable on struct member types instead of the memory object
        // declarations it was meant to. Workaround this by checking that if all the struct members are non-writable,
        // make the global variable constant.
        bool allReadOnly = true;
        for (uint32_t i = 0; i < pSpvVarType->getStructMemberCount(); i++)
        {
            if (pSpvVarType->hasMemberDecorate(i, DecorationNonWritable) == false)
            {
                allReadOnly = false;
                break;
            }
        }

        if (allReadOnly)
        {
            readOnly = true;
        }
    }

    uint32_t addrSpace = pPtrType->getPointerAddressSpace();
    string varName = pSpvVar->getName();

    GlobalVariable* const pGlobalVar = new GlobalVariable(*M,
                                                          pVarType,
                                                          readOnly,
                                                          GlobalValue::ExternalLinkage,
                                                          pInitializer,
                                                          varName,
                                                          nullptr,
                                                          GlobalVariable::NotThreadLocal,
                                                          addrSpace);

    if (addrSpace == SPIRAS_Local)
    {
        pGlobalVar->setAlignment(MaybeAlign(16));

        // NOTE: Give shared variable a name to skip "global optimize pass".
        // The pass will change constant store operations to initializerand this
        // is disallowed in backend compiler.
        if (!pGlobalVar->hasName())
        {
            pGlobalVar->setName("lds");
        }
    }

    SPIRVBuiltinVariableKind builtinKind;
    if (pSpvVar->isBuiltin(&builtinKind))
    {
        BuiltinGVMap[pGlobalVar] = builtinKind;
    }

    return pGlobalVar;
}

// =====================================================================================================================
// Handle OpTranspose.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpTranspose>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstTemplateBase* const pSpvTranpose = static_cast<SPIRVInstTemplateBase*>(pSpvValue);

    Value* const pMatrix = transValue(pSpvTranpose->getOpValue(0),
                                      getBuilder()->GetInsertBlock()->getParent(),
                                      getBuilder()->GetInsertBlock());
    return getBuilder()->CreateTransposeMatrix(pMatrix);
}

// =====================================================================================================================
// Handle OpMatrixTimesScalar.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpMatrixTimesScalar>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pMatrix = transValue(spvOperands[0], pFunc, pBlock);
    Value* const pScalar = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateMatrixTimesScalar(pMatrix, pScalar);
}

// =====================================================================================================================
// Handle OpVectorTimesMatrix.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpVectorTimesMatrix>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pVector = transValue(spvOperands[0], pFunc, pBlock);
    Value* const pMatrix = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateVectorTimesMatrix(pVector, pMatrix);
}

// =====================================================================================================================
// Handle OpMatrixTimesVector.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpMatrixTimesVector>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pMatrix = transValue(spvOperands[0], pFunc, pBlock);
    Value* const pVector = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateMatrixTimesVector(pMatrix, pVector);
}

// =====================================================================================================================
// Handle OpMatrixTimesMatrix.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpMatrixTimesMatrix>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pMatrix1 = transValue(spvOperands[0], pFunc, pBlock);
    Value* const pMatrix2 = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateMatrixTimesMatrix(pMatrix1, pMatrix2);
}

// =====================================================================================================================
// Handle OpOuterProduct.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpOuterProduct>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pVector1 = transValue(spvOperands[0], pFunc, pBlock);
    Value* const pVector2 = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateOuterProduct(pVector1, pVector2);
}

// =====================================================================================================================
// Handle OpDot.
template<> Value* SPIRVToLLVM::transValueWithOpcode<OpDot>(
    SPIRVValue* const pSpvValue) // [in] A SPIR-V value.
{
    SPIRVInstruction* const pSpvInst = static_cast<SPIRVInstruction*>(pSpvValue);
    std::vector<SPIRVValue*> spvOperands = pSpvInst->getOperands();
    BasicBlock* const pBlock = getBuilder()->GetInsertBlock();
    Function* const pFunc = getBuilder()->GetInsertBlock()->getParent();
    Value* const pVector1 = transValue(spvOperands[0], pFunc, pBlock);
    Value* const pVector2 = transValue(spvOperands[1], pFunc, pBlock);
    return getBuilder()->CreateDotProduct(pVector1, pVector2);
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
    auto BTy = BV->getType();
    auto NullPtrTy = transType(BTy);
    Value *NullPtr = nullptr;
    // For local memory space (LDS) the NULL value is 0xFFFFFFFF, not 0x0.
    if (BTy->isTypePointer() && BTy->getPointerStorageClass() == spv::StorageClassWorkgroup) {
      auto NullPtrAsInt = getBuilder()->getInt32(0xFFFFFFFF);
      NullPtr = getBuilder()->CreateIntToPtr(NullPtrAsInt, NullPtrTy);
    } else {
      NullPtr = Constant::getNullValue(NullPtrTy);
    }
    return mapValue(BV, NullPtr);
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
      return mapValue(BV, ConstantArray::get(
          dyn_cast<ArrayType>(transType(BCC->getType())), CV));
    case OpTypeStruct: {
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

  case OpSpecConstantOp: {
    auto BI = static_cast<SPIRVSpecConstantOp*>(BV)->getMappedConstant();
    return mapValue(BV, transValue(BI, nullptr, nullptr, false));
  }

  case OpUndef:
    return mapValue(BV, UndefValue::get(transType(BV->getType())));

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

#define HANDLE_OPCODE(op) case (op):  \
  if (BB) {                           \
    getBuilder()->SetInsertPoint(BB); \
    updateBuilderDebugLoc(BV, F);     \
  }                                   \
  return mapValue(BV, transValueWithOpcode<op>(BV))

  HANDLE_OPCODE(OpVariable);

#undef HANDLE_OPCODE

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
  if (BB) {
    getBuilder()->SetInsertPoint(BB);
    updateBuilderDebugLoc(BV, F);
    setFastMathFlags(BV);
  }

  switch (static_cast<uint32_t>(BV->getOpCode())) {
  case OpBranch: {
    auto BR = static_cast<SPIRVBranch *>(BV);
    auto Successor = cast<BasicBlock>(transValue(BR->getTargetLabel(), F, BB));
    auto BI = BranchInst::Create(Successor, BB);
    auto LM = static_cast<SPIRVLoopMerge *>(BR->getPrevious());
    if (LM != nullptr && LM->getOpCode() == OpLoopMerge)
      setLLVMLoopMetadata(LM, BI);
    else if (BR->getBasicBlock()->getLoopMerge())
      setLLVMLoopMetadata(BR->getBasicBlock()->getLoopMerge(), BI);

    recordBlockPredecessor(Successor, BB);
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

    auto TrueSuccessor =
      cast<BasicBlock>(transValue(BR->getTrueLabel(), F, BB));
    auto FalseSuccessor =
      cast<BasicBlock>(transValue(BR->getFalseLabel(), F, BB));
    auto BC = BranchInst::Create(TrueSuccessor, FalseSuccessor, C, BB);
    auto LM = static_cast<SPIRVLoopMerge *>(BR->getPrevious());
    if (LM != nullptr && LM->getOpCode() == OpLoopMerge)
      setLLVMLoopMetadata(LM, BC);
    else if (BR->getBasicBlock()->getLoopMerge())
      setLLVMLoopMetadata(BR->getBasicBlock()->getLoopMerge(), BC);

    recordBlockPredecessor(TrueSuccessor, BB);
    recordBlockPredecessor(FalseSuccessor, BB);
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

#ifndef NDEBUG
    SmallDenseSet<BasicBlock*, 4> SeenPredecessors;
#endif
    Phi->foreachPair([&](SPIRVValue *IncomingV, SPIRVBasicBlock *IncomingBB,
                         size_t Index) {
      auto TranslatedVal = transValue(IncomingV, F, BB);
      auto TranslatedBB = cast<BasicBlock>(transValue(IncomingBB, F, BB));
      LPhi->addIncoming(TranslatedVal, TranslatedBB);

#ifndef NDEBUG
      assert(SeenPredecessors.count(TranslatedBB) == 0 &&
             "SPIR-V requires phi entries to be unique for duplicate predecessor blocks.");
      SeenPredecessors.insert(TranslatedBB);
#endif
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

  case OpSelect: {
    SPIRVSelect *BS = static_cast<SPIRVSelect *>(BV);
    return mapValue(BV,
                    SelectInst::Create(transValue(BS->getCondition(), F, BB),
                                       transValue(BS->getTrueValue(), F, BB),
                                       transValue(BS->getFalseValue(), F, BB),
                                       BV->getName(), BB));
  }

  case OpLine:
  case OpSelectionMerge:
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

          auto Successor = cast<BasicBlock>(transValue(Label, F, BB));
          LS->addCase(ConstantInt::get(dyn_cast<IntegerType>(Select->getType()),
                                       Literal),
                      Successor);
          recordBlockPredecessor(Successor, BB);
        });
    return mapValue(BV, LS);
  }

  case OpVectorTimesScalar: {
    auto VTS = static_cast<SPIRVVectorTimesScalar *>(BV);
    auto Scalar = transValue(VTS->getScalar(), F, BB);
    auto Vector = transValue(VTS->getVector(), F, BB);
    assert(Vector->getType()->isVectorTy() && "Invalid type");
    unsigned VecSize = Vector->getType()->getVectorNumElements();
    auto NewVec = getBuilder()->CreateVectorSplat(VecSize, Scalar, Scalar->getName());
    NewVec->takeName(Scalar);
    auto Scale = getBuilder()->CreateFMul(Vector, NewVec, "scale");
    return mapValue(BV, Scale);
  }

#if SPV_VERSION >= 0x10400
  case OpCopyObject:
  case OpCopyLogical: {
#else
  case OpCopyObject: {
#endif
    SPIRVCopyBase *Copy = static_cast<SPIRVCopyBase *>(BV);
    AllocaInst* AI = nullptr;
    // NOTE: Alloc instructions not in the entry block will prevent LLVM from doing function
    // inlining. Try to move those alloc instructions to the entry block.
    auto FirstInst = BB->getParent()->getEntryBlock().getFirstInsertionPt();
    if (FirstInst != BB->getParent()->getEntryBlock().end())
      AI = new AllocaInst(transType(Copy->getOperand()->getType()),
                          M->getDataLayout().getAllocaAddrSpace(),
                          "",
                          &*FirstInst);
    else
      AI = new AllocaInst(transType(Copy->getOperand()->getType()),
                          M->getDataLayout().getAllocaAddrSpace(),
                          "",
                          BB);

    new StoreInst(transValue(Copy->getOperand(), F, BB), AI, BB);
    LoadInst *LI = new LoadInst(AI, "", BB);
    return mapValue(BV, LI);
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
        V = InsertValueInst::Create(V, Constituents[I], I, "", BB);
      }
      return mapValue(BV, V);
    }
    case OpTypeMatrix: {
      auto BVTy = BV->getType();
      auto MatClmTy = transType(BVTy->getMatrixColumnType());
      auto MatCount = BVTy->getMatrixColumnCount();
      auto MatTy = ArrayType::get(MatClmTy, MatCount);

      Value* V = UndefValue::get(MatTy);
      for (uint32_t I = 0, E = Constituents.size(); I < E; ++I) {
          V = InsertValueInst::Create(V, Constituents[I], I, "", BB);
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
        return mapValue(BV, ExtractElementInst::Create(
          V, ConstantInt::get(*Context, APInt(32, LastIdx)),
          BV->getName(), BB));
      } else
        return mapValue(BV, ExtractValueInst::Create(
          CV, CE->getIndices(), BV->getName(), BB));
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
            BV, InsertValueInst::Create(CV, V, Idxs, BV->getName(), BB));
      } else
        return mapValue(
            BV, InsertValueInst::Create(
                    CV, transValue(CI->getObject(), F, BB),
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
    SmallVector<Value *, 8> Args;
    for (SPIRVValue *BArg : BC->getArgumentValues()) {
      Value *Arg = transValue(BArg, F, BB);
      if (!Arg) {
        // This arg is a variable that is (array of) image/sampler/sampledimage.
        // Materialize it.
        assert(BArg->getOpCode() == OpVariable);
        Arg = transImagePointer(BArg);
      }
      Args.push_back(Arg);
    }
    auto Call =
        CallInst::Create(transFunction(BC->getFunction()), Args, "", BB);
    setCallingConv(Call);
    setAttrByCalledFunc(Call);
    return mapValue(BV, Call);
  }

  case OpControlBarrier:
  case OpMemoryBarrier:
    return mapValue(
        BV, transBarrierFence(static_cast<SPIRVInstruction *>(BV), BB));

  case OpSNegate: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    return mapValue(
        BV, BinaryOperator::CreateNSWNeg(transValue(BC->getOperand(0), F, BB),
                                         BV->getName(), BB));
  }
  case OpSMod: {
    SPIRVBinary *BC = static_cast<SPIRVBinary *>(BV);
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    Value *Val1 = transValue(BC->getOperand(1), F, BB);
    return mapValue(BC, getBuilder()->CreateSMod(Val0, Val1));
  }
  case OpFMod: {
    SPIRVFMod *BC = static_cast<SPIRVFMod *>(BV);
    Value *Val0 = transValue(BC->getDividend(), F, BB);
    Value *Val1 = transValue(BC->getDivisor(), F, BB);
    return mapValue(BC, getBuilder()->CreateFMod(Val0, Val1));
  }
  case OpFNegate: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    // Implement -x as -0.0 - x.
    Value *NegZero = ConstantFP::getNegativeZero(transType(BC->getType()));
    auto FNeg = BinaryOperator::CreateFSub(NegZero,
                                           transValue(BC->getOperand(0), F, BB),
                                           BV->getName(), BB);
    setFastMathFlags(FNeg);
    return mapValue(BV, FNeg);
  }

  case OpFConvert: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    Value *Val = transValue(BC->getOperand(0), F, BB);
    Type *DestTy = transType(BC->getType());
    if (Val->getType()->getScalarType()->getPrimitiveSizeInBits() <=
        DestTy->getScalarType()->getPrimitiveSizeInBits())
      return mapValue(BV, getBuilder()->CreateFPExt(Val, DestTy));

    // TODO: use hardcoded values during namespace flux for llvm
    //fp::RoundingMode RM = fp::rmDynamic;
    unsigned RM = 0; // fp::rmDynamic
    SPIRVFPRoundingModeKind Rounding;
    if (BC->hasFPRoundingMode(&Rounding)) {
      switch (Rounding) {
      case FPRoundingModeRTE:
        // TODO: use hardcoded values during namespace flux for llvm
        //RM = fp::rmToNearest;
        RM = 1;
        break;
      case FPRoundingModeRTZ:
        //RM = fp::rmTowardZero;
        RM = 4;
        break;
      case FPRoundingModeRTP:
        //RM = fp::rmUpward;
        RM = 3;
        break;
      case FPRoundingModeRTN:
        //RM = fp::rmDownward;
        RM = 2;
        break;
      default:
        llvm_unreachable("Should never be called!");
      }
      return mapValue(BV, getBuilder()->CreateFpTruncWithRounding(Val, DestTy, RM));
    }
    return mapValue(BV, getBuilder()->CreateFPTrunc(Val, DestTy));
  }

  case OpBitCount: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    Value *Val = transValue(BC->getOperand(0), F, BB);
    Value *Result = getBuilder()->CreateUnaryIntrinsic(Intrinsic::ctpop, Val);
    Result = getBuilder()->CreateZExtOrTrunc(Result, transType(BC->getType()));
    return mapValue(BV, Result);
  }

  case OpBitReverse: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    Value *Val = transValue(BC->getOperand(0), F, BB);
    Value *Result =
        getBuilder()->CreateUnaryIntrinsic(Intrinsic::bitreverse, Val);
    return mapValue(BV, Result);
  }

  case OpBitFieldInsert: {
    auto BC = static_cast<SPIRVInstTemplateBase *>(BV);
    Value *Base = transValue(BC->getOperand(0), F, BB);
    Value *Insert = transValue(BC->getOperand(1), F, BB);
    Value *Offset = transValue(BC->getOperand(2), F, BB);
    Value *Count = transValue(BC->getOperand(3), F, BB);
    return mapValue(
        BV, getBuilder()->CreateInsertBitField(Base, Insert, Offset, Count));
  }

  case OpBitFieldUExtract:
  case OpBitFieldSExtract: {
    auto BC = static_cast<SPIRVInstTemplateBase *>(BV);
    Value *Base = transValue(BC->getOperand(0), F, BB);
    bool IsSigned = (OC == OpBitFieldSExtract);
    Value *Offset = transValue(BC->getOperand(1), F, BB);
    Value *Count = transValue(BC->getOperand(2), F, BB);
    return mapValue(
        BV, getBuilder()->CreateExtractBitField(Base, Offset, Count, IsSigned));
  }

  case OpQuantizeToF16: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    Value *Val = transValue(BC->getOperand(0), F, BB);
    Value *Result = getBuilder()->CreateQuantizeToFp16(Val);
    return mapValue(BC, Result);
  }

  case OpLogicalNot:
  case OpNot: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    return mapValue(
        BV, BinaryOperator::CreateNot(transValue(BC->getOperand(0), F, BB),
                                      BV->getName(), BB));
  }

  case OpAll:
  case OpAny: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    Value *Val = transValue(BC->getOperand(0), F, BB);
    if (!isa<VectorType>(Val->getType()))
      return Val;
    Value *Result = getBuilder()->CreateExtractElement(Val, uint64_t(0));
    for (unsigned I = 1, E = Val->getType()->getVectorNumElements(); I != E;
         ++I) {
      Value *Elem = getBuilder()->CreateExtractElement(Val, I);
      if (OC == OpAny)
        Result = getBuilder()->CreateOr(Result, Elem);
      else
        Result = getBuilder()->CreateAnd(Result, Elem);
    }
    // Vector of bool is <N x i32>, but single bool result needs to be i1.
    Result = getBuilder()->CreateTrunc(Result, transType(BC->getType()));
    return mapValue(BC, Result);
  }

  case OpIAddCarry: {
    SPIRVBinary *BC = static_cast<SPIRVBinary *>(BV);
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    Value *Val1 = transValue(BC->getOperand(1), F, BB);
    Value *Add = getBuilder()->CreateIntrinsic(Intrinsic::uadd_with_overflow,
                                               Val0->getType(), {Val0, Val1});
    Value *Result = UndefValue::get(transType(BC->getType()));
    Result = getBuilder()->CreateInsertValue(
        Result, getBuilder()->CreateExtractValue(Add, 0), 0);
    Result = getBuilder()->CreateInsertValue(
        Result,
        getBuilder()->CreateZExt(getBuilder()->CreateExtractValue(Add, 1),
                                 Val0->getType()),
        1);
    return mapValue(BC, Result);
  }

  case OpISubBorrow: {
    SPIRVBinary *BC = static_cast<SPIRVBinary *>(BV);
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    Value *Val1 = transValue(BC->getOperand(1), F, BB);
    Value *Sub = getBuilder()->CreateIntrinsic(Intrinsic::usub_with_overflow,
                                               Val0->getType(), {Val0, Val1});
    Value *Result = UndefValue::get(transType(BC->getType()));
    Result = getBuilder()->CreateInsertValue(
        Result, getBuilder()->CreateExtractValue(Sub, 0), 0);
    Result = getBuilder()->CreateInsertValue(
        Result,
        getBuilder()->CreateZExt(getBuilder()->CreateExtractValue(Sub, 1),
                                 Val0->getType()),
        1);
    return mapValue(BC, Result);
  }

  case OpUMulExtended:
  case OpSMulExtended: {
    SPIRVBinary *BC = static_cast<SPIRVBinary *>(BV);
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    Value *Val1 = transValue(BC->getOperand(1), F, BB);
    Type *InTy = Val0->getType();
    Type *ExtendedTy = Llpc::Builder::GetConditionallyVectorizedTy(
        getBuilder()->getInt64Ty(), Val0->getType());
    if (OC == OpUMulExtended) {
      Val0 = getBuilder()->CreateZExt(Val0, ExtendedTy);
      Val1 = getBuilder()->CreateZExt(Val1, ExtendedTy);
    } else {
      Val0 = getBuilder()->CreateSExt(Val0, ExtendedTy);
      Val1 = getBuilder()->CreateSExt(Val1, ExtendedTy);
    }
    Value *Mul = getBuilder()->CreateMul(Val0, Val1);
    Value *LoResult = getBuilder()->CreateTrunc(Mul, InTy);
    Value *HiResult = getBuilder()->CreateTrunc(
        getBuilder()->CreateLShr(Mul, ConstantInt::get(Mul->getType(), 32)),
        InTy);
    Value *Result = UndefValue::get(transType(BC->getType()));
    Result = getBuilder()->CreateInsertValue(Result, LoResult, 0);
    Result = getBuilder()->CreateInsertValue(Result, HiResult, 1);
    return mapValue(BC, Result);
  }

  case OpIsInf: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    Value *Result = getBuilder()->CreateIsInf(Val0);
    // ZExt to cope with vector of bool being represented by <N x i32>
    return mapValue(BV,
                    getBuilder()->CreateZExt(Result, transType(BC->getType())));
  }

  case OpIsNan: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    Value *Result = getBuilder()->CreateIsNaN(Val0);
    // ZExt to cope with vector of bool being represented by <N x i32>
    return mapValue(BV,
                    getBuilder()->CreateZExt(Result, transType(BC->getType())));
  }

  case OpDPdx:
  case OpDPdxCoarse:
  case OpDPdxFine: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    bool IsFine = OC == OpDPdxFine;
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    return mapValue(
        BV, getBuilder()->CreateDerivative(Val0, /*isY=*/false, IsFine));
  }

  case OpDPdy:
  case OpDPdyCoarse:
  case OpDPdyFine: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    bool IsFine = OC == OpDPdyFine;
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    return mapValue(BV,
                    getBuilder()->CreateDerivative(Val0, /*isY=*/true, IsFine));
  }

  case OpFwidth:
  case OpFwidthCoarse:
  case OpFwidthFine: {
    SPIRVUnary *BC = static_cast<SPIRVUnary *>(BV);
    bool IsFine = OC == OpFwidthFine;
    Value *Val0 = transValue(BC->getOperand(0), F, BB);
    Value *Dpdx = getBuilder()->CreateDerivative(Val0, /*isY=*/false, IsFine);
    Value *Dpdy = getBuilder()->CreateDerivative(Val0, /*isY=*/true, IsFine);
    Value *AbsDpdx = getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, Dpdx);
    Value *AbsDpdy = getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, Dpdy);
    return mapValue(BV, getBuilder()->CreateFAdd(AbsDpdx, AbsDpdy));
  }

  case OpImageSampleImplicitLod:
  case OpImageSampleExplicitLod:
  case OpImageSampleDrefImplicitLod:
  case OpImageSampleDrefExplicitLod:
  case OpImageSampleProjImplicitLod:
  case OpImageSampleProjExplicitLod:
  case OpImageSampleProjDrefImplicitLod:
  case OpImageSampleProjDrefExplicitLod:
  case OpImageSparseSampleImplicitLod:
  case OpImageSparseSampleExplicitLod:
  case OpImageSparseSampleDrefImplicitLod:
  case OpImageSparseSampleDrefExplicitLod:
  case OpImageSparseSampleProjImplicitLod:
  case OpImageSparseSampleProjExplicitLod:
  case OpImageSparseSampleProjDrefImplicitLod:
  case OpImageSparseSampleProjDrefExplicitLod:
    return mapValue(BV,
        transSPIRVImageSampleFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpImageFetch:
  case OpImageSparseFetch:
  case OpImageRead:
  case OpImageSparseRead:
    return mapValue(BV,
        transSPIRVImageFetchReadFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpImageGather:
  case OpImageDrefGather:
  case OpImageSparseGather:
  case OpImageSparseDrefGather:
    return mapValue(BV,
        transSPIRVImageGatherFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpImageQuerySizeLod:
  case OpImageQuerySize:
    return mapValue(BV,
        transSPIRVImageQuerySizeFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpImageQueryLod:
    return mapValue(BV,
        transSPIRVImageQueryLodFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpImageQueryLevels:
    return mapValue(BV,
        transSPIRVImageQueryLevelsFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpImageQuerySamples:
    return mapValue(BV,
        transSPIRVImageQuerySamplesFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpImageWrite:
    return mapValue(BV,
        transSPIRVImageWriteFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpFragmentMaskFetchAMD:
    return mapValue(BV,
        transSPIRVFragmentMaskFetchFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpFragmentFetchAMD:
    return mapValue(BV,
        transSPIRVFragmentFetchFromInst(
            static_cast<SPIRVInstruction *>(BV),
            BB));

  case OpImageSparseTexelsResident: {
    SPIRVImageSparseTexelsResident *BI = static_cast<SPIRVImageSparseTexelsResident *>(BV);
    auto ResidentCode = transValue(BI->getResidentCode(), F, BB);
    return mapValue(BV, getBuilder()->CreateICmpEQ(ResidentCode, getBuilder()->getInt32(0)));
  }
  case OpImageTexelPointer:
    return nullptr;
#if SPV_VERSION >= 0x10400
  case OpPtrDiff: {
    SPIRVBinary *const BI = static_cast<SPIRVBinary *>(BV);
    Value *const Op1 = transValue(BI->getOpValue(0),
                                  getBuilder()->GetInsertBlock()->getParent(),
                                  getBuilder()->GetInsertBlock());
    Value *const Op2 = transValue(BI->getOpValue(1),
                                  getBuilder()->GetInsertBlock()->getParent(),
                                  getBuilder()->GetInsertBlock());

    Value *PtrDiff = getBuilder()->CreatePtrDiff(Op1, Op2);

    auto DestType = dyn_cast<IntegerType>(transType(BV->getType()));
    auto PtrDiffType = dyn_cast<IntegerType>(PtrDiff->getType());
    assert(DestType->getBitWidth() <= PtrDiffType->getBitWidth());
    if (DestType->getBitWidth() < PtrDiffType->getBitWidth())
        PtrDiff = new TruncInst(PtrDiff, DestType, "", BB);

    return mapValue(BV, PtrDiff);
  }
#endif

#define HANDLE_OPCODE(op) case (op): \
  return mapValue(BV, transValueWithOpcode<op>(BV))

  HANDLE_OPCODE(OpAtomicLoad);
  HANDLE_OPCODE(OpAtomicStore);
  HANDLE_OPCODE(OpAtomicExchange);
  HANDLE_OPCODE(OpAtomicCompareExchange);
  HANDLE_OPCODE(OpAtomicIIncrement);
  HANDLE_OPCODE(OpAtomicIDecrement);
  HANDLE_OPCODE(OpAtomicIAdd);
  HANDLE_OPCODE(OpAtomicISub);
  HANDLE_OPCODE(OpAtomicSMin);
  HANDLE_OPCODE(OpAtomicUMin);
  HANDLE_OPCODE(OpAtomicSMax);
  HANDLE_OPCODE(OpAtomicUMax);
  HANDLE_OPCODE(OpAtomicAnd);
  HANDLE_OPCODE(OpAtomicOr);
  HANDLE_OPCODE(OpAtomicXor);
  HANDLE_OPCODE(OpCopyMemory);
  HANDLE_OPCODE(OpLoad);
  HANDLE_OPCODE(OpStore);
  HANDLE_OPCODE(OpEndPrimitive);
  HANDLE_OPCODE(OpEndStreamPrimitive);
  HANDLE_OPCODE(OpAccessChain);
  HANDLE_OPCODE(OpArrayLength);
  HANDLE_OPCODE(OpInBoundsAccessChain);
  HANDLE_OPCODE(OpPtrAccessChain);
  HANDLE_OPCODE(OpInBoundsPtrAccessChain);
  HANDLE_OPCODE(OpImage);
  HANDLE_OPCODE(OpSampledImage);
  HANDLE_OPCODE(OpKill);
  HANDLE_OPCODE(OpReadClockKHR);
  HANDLE_OPCODE(OpGroupAll);
  HANDLE_OPCODE(OpGroupAny);
  HANDLE_OPCODE(OpGroupBroadcast);
  HANDLE_OPCODE(OpGroupIAdd);
  HANDLE_OPCODE(OpGroupFAdd);
  HANDLE_OPCODE(OpGroupFMin);
  HANDLE_OPCODE(OpGroupUMin);
  HANDLE_OPCODE(OpGroupSMin);
  HANDLE_OPCODE(OpGroupFMax);
  HANDLE_OPCODE(OpGroupUMax);
  HANDLE_OPCODE(OpGroupSMax);
  HANDLE_OPCODE(OpGroupNonUniformElect);
  HANDLE_OPCODE(OpGroupNonUniformAll);
  HANDLE_OPCODE(OpGroupNonUniformAny);
  HANDLE_OPCODE(OpGroupNonUniformAllEqual);
  HANDLE_OPCODE(OpGroupNonUniformBroadcast);
  HANDLE_OPCODE(OpGroupNonUniformBroadcastFirst);
  HANDLE_OPCODE(OpGroupNonUniformBallot);
  HANDLE_OPCODE(OpGroupNonUniformInverseBallot);
  HANDLE_OPCODE(OpGroupNonUniformBallotBitExtract);
  HANDLE_OPCODE(OpGroupNonUniformBallotBitCount);
  HANDLE_OPCODE(OpGroupNonUniformBallotFindLSB);
  HANDLE_OPCODE(OpGroupNonUniformBallotFindMSB);
  HANDLE_OPCODE(OpGroupNonUniformShuffle);
  HANDLE_OPCODE(OpGroupNonUniformShuffleXor);
  HANDLE_OPCODE(OpGroupNonUniformShuffleUp);
  HANDLE_OPCODE(OpGroupNonUniformShuffleDown);
  HANDLE_OPCODE(OpGroupNonUniformIAdd);
  HANDLE_OPCODE(OpGroupNonUniformFAdd);
  HANDLE_OPCODE(OpGroupNonUniformIMul);
  HANDLE_OPCODE(OpGroupNonUniformFMul);
  HANDLE_OPCODE(OpGroupNonUniformSMin);
  HANDLE_OPCODE(OpGroupNonUniformUMin);
  HANDLE_OPCODE(OpGroupNonUniformFMin);
  HANDLE_OPCODE(OpGroupNonUniformSMax);
  HANDLE_OPCODE(OpGroupNonUniformUMax);
  HANDLE_OPCODE(OpGroupNonUniformFMax);
  HANDLE_OPCODE(OpGroupNonUniformBitwiseAnd);
  HANDLE_OPCODE(OpGroupNonUniformBitwiseOr);
  HANDLE_OPCODE(OpGroupNonUniformBitwiseXor);
  HANDLE_OPCODE(OpGroupNonUniformLogicalAnd);
  HANDLE_OPCODE(OpGroupNonUniformLogicalOr);
  HANDLE_OPCODE(OpGroupNonUniformLogicalXor);
  HANDLE_OPCODE(OpGroupNonUniformQuadBroadcast);
  HANDLE_OPCODE(OpGroupNonUniformQuadSwap);
  HANDLE_OPCODE(OpSubgroupBallotKHR);
  HANDLE_OPCODE(OpSubgroupFirstInvocationKHR);
  HANDLE_OPCODE(OpSubgroupAllKHR);
  HANDLE_OPCODE(OpSubgroupAnyKHR);
  HANDLE_OPCODE(OpSubgroupAllEqualKHR);
  HANDLE_OPCODE(OpSubgroupReadInvocationKHR);
  HANDLE_OPCODE(OpGroupIAddNonUniformAMD);
  HANDLE_OPCODE(OpGroupFAddNonUniformAMD);
  HANDLE_OPCODE(OpGroupFMinNonUniformAMD);
  HANDLE_OPCODE(OpGroupUMinNonUniformAMD);
  HANDLE_OPCODE(OpGroupSMinNonUniformAMD);
  HANDLE_OPCODE(OpGroupFMaxNonUniformAMD);
  HANDLE_OPCODE(OpGroupUMaxNonUniformAMD);
  HANDLE_OPCODE(OpGroupSMaxNonUniformAMD);
  HANDLE_OPCODE(OpTranspose);
  HANDLE_OPCODE(OpExtInst);
  HANDLE_OPCODE(OpMatrixTimesScalar);
  HANDLE_OPCODE(OpVectorTimesMatrix);
  HANDLE_OPCODE(OpMatrixTimesVector);
  HANDLE_OPCODE(OpMatrixTimesMatrix);
  HANDLE_OPCODE(OpOuterProduct);
  HANDLE_OPCODE(OpDot);
  HANDLE_OPCODE(OpDemoteToHelperInvocationEXT);
  HANDLE_OPCODE(OpIsHelperInvocationEXT);

#undef HANDLE_OPCODE

  default: {
    auto OC = BV->getOpCode();
    if (isSPIRVCmpInstTransToLLVMInst(static_cast<SPIRVInstruction *>(BV))) {
      return mapValue(BV, transCmpInst(BV, BB, F));
    } else if (isBinaryShiftLogicalBitwiseOpCode(OC) ||
                isLogicalOpCode(OC)) {
      return mapValue(BV, transShiftLogicalBitwiseInst(BV, BB, F));
    } else if (isCvtOpCode(OC)) {
      Value *Inst = transConvertInst(BV, F, BB);
      return mapValue(BV, Inst);
    }
    return mapValue(
        BV, transSPIRVBuiltinFromInst(static_cast<SPIRVInstruction *>(BV), BB));
  }

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

    SPIRVWord MaxOffset = 0;
    if (BA->hasDecorate(DecorationMaxByteOffset, 0, &MaxOffset)) {
      AttrBuilder Builder;
      Builder.addDereferenceableAttr(MaxOffset);
      I->addAttrs(Builder);
    }
  }

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

  // Update phi nodes -- add missing incoming arcs.
  // This is necessary because LLVM's CFG is a multigraph, while SPIR-V's
  // CFG is not.
  for (BasicBlock &BB : *F) {

    // Add missing incoming arcs to each phi node that requires fixups.
    for (PHINode &Phi : BB.phis()) {
      const unsigned InitialNumIncoming = Phi.getNumIncomingValues();
      for (unsigned i = 0; i != InitialNumIncoming; ++i) {
        BasicBlock* Predecessor = Phi.getIncomingBlock(i);
        Value *IncomingValue = Phi.getIncomingValue(i);
        const unsigned NumIncomingArcsForPred =
          getBlockPredecessorCounts(&BB, Predecessor);

        for (unsigned j = 1; j < NumIncomingArcsForPred; ++j)
          Phi.addIncoming(IncomingValue, Predecessor);
      }
    }
  }

  BlockPredecessorToCount.clear();

  return F;
}

// Prints LLVM-style name for type to raw_ostream
static void printTypeName(Type *Ty, raw_ostream &NameStream) {
  if (auto PtrTy = dyn_cast<PointerType>(Ty)) {
    NameStream << "p";
    if (PtrTy->getAddressSpace())
      NameStream << PtrTy->getAddressSpace();
    Ty = PtrTy->getPointerElementType();
  }
  if (auto VecTy = dyn_cast<VectorType>(Ty)) {
    NameStream << "v" << VecTy->getNumElements();
    Ty = VecTy->getElementType();
  }
  if (Ty->isFloatingPointTy()) {
    NameStream << "f" << Ty->getScalarSizeInBits();
    return;
  }
  if (Ty->isIntegerTy()) {
    NameStream << "i" << Ty->getScalarSizeInBits();
    return;
  }
  assert(Ty->isVoidTy());
  NameStream << "V";
}

// Adds LLVM-style type mangling suffix for the specified return type and args
// to the name. This is used when adding a call to an external function that
// is later lowered in a SPIRVLower* pass.
//
// @param RetTy : Return type or nullptr
// @param Args : Arg values
// @param [out] Name : String to append the type mangling to
static void appendTypeMangling(Type *RetTy, ArrayRef<Value *> Args, std::string &Name) {
  raw_string_ostream NameStream(Name);
  if (RetTy && !RetTy->isVoidTy()) {
    NameStream << ".";
    printTypeName(RetTy, NameStream);
  }
  for (auto Arg : Args) {
    NameStream << ".";
    printTypeName(Arg->getType(), NameStream);
  }
}

Instruction * SPIRVToLLVM::transBuiltinFromInst(const std::string& FuncName,
                                                SPIRVInstruction* BI,
                                                BasicBlock* BB) {
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
  bool HasFuncPtrArg = false;
  for (auto& I:ArgTys) {
    if (isa<FunctionType>(I)) {
      I = PointerType::get(I, SPIRAS_Private);
      HasFuncPtrArg = true;
    }
  }
  std::string MangledName(FuncName);
  appendTypeMangling(nullptr, Args, MangledName);
  Function* Func = M->getFunction(MangledName);
  FunctionType* FT = FunctionType::get(RetTy, ArgTys, false);
  // ToDo: Some intermediate functions have duplicate names with
  // different function types. This is OK if the function name
  // is used internally and finally translated to unique function
  // names. However it is better to have a way to differentiate
  // between intermidiate functions and final functions and make
  // sure final functions have unique names.
  if (!Func || Func->getFunctionType() != FT) {
    Func = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    Func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);
  }
  auto Call = CallInst::Create(Func, Args, "", BB);
  setName(Call, BI);
  setAttrByCalledFunc(Call);
  return Call;
}

// =============================================================================
// Convert SPIR-V dimension and arrayed into Builder dimension.
static unsigned convertDimension(const SPIRVTypeImageDescriptor *Desc) {
  if (Desc->MS) {
    assert(Desc->Dim == Dim2D || Desc->Dim == DimSubpassData);
    return !Desc->Arrayed ? Llpc::Builder::Dim2DMsaa
                          : Llpc::Builder::Dim2DArrayMsaa;
  }
  if (!Desc->Arrayed) {
    switch (static_cast<uint32_t>(Desc->Dim)) {
    case Dim1D:
      return Llpc::Builder::Dim1D;
    case DimBuffer:
      return Llpc::Builder::Dim1D;
    case Dim2D:
      return Llpc::Builder::Dim2D;
    case DimRect:
      return Llpc::Builder::Dim2D;
    case DimCube:
      return Llpc::Builder::DimCube;
    case Dim3D:
      return Llpc::Builder::Dim3D;
    case DimSubpassData:
      return Llpc::Builder::Dim2D;
    default:
      break;
    }
  } else {
    switch (static_cast<uint32_t>(Desc->Dim)) {
    case Dim1D:
      return Llpc::Builder::Dim1DArray;
    case DimBuffer:
      return Llpc::Builder::Dim1DArray;
    case Dim2D:
      return Llpc::Builder::Dim2DArray;
    case DimRect:
      return Llpc::Builder::Dim2DArray;
    case DimCube:
      return Llpc::Builder::DimCubeArray;
    default:
      break;
    }
  }
  llvm_unreachable("Unhandled image dimension");
  return 0;
}

// =============================================================================
// Get image and/or sampler descriptors, and get information from the image
// type.
void SPIRVToLLVM::getImageDesc(SPIRVValue *BImageInst,
                               ExtractedImageInfo *Info) {
  if (BImageInst->hasDecorate(DecorationNonUniformEXT))
    Info->Flags |= Llpc::Builder::ImageFlagNonUniformImage;

  if (BImageInst->getOpCode() == OpImageTexelPointer) {
    // We are looking at the OpImageTexelPointer for an image atomic. Load the
    // image descriptor from its image pointer.
    SPIRVValue *BImagePtr =
        static_cast<SPIRVImageTexelPointer *>(BImageInst)->getImage();
    Info->Desc = &static_cast<SPIRVTypeImage *>(
                      BImagePtr->getType()->getPointerElementType())
                      ->getDescriptor();
    Info->Dim = convertDimension(Info->Desc);
    Info->ImageDesc = transLoadImage(BImagePtr);
    if (isa<StructType>(Info->ImageDesc->getType())) {
      // Extract image descriptor from struct containing image+fmask descs.
      Info->ImageDesc =
          getBuilder()->CreateExtractValue(Info->ImageDesc, uint64_t(0));
    }
    // We also need to trace back to the OpVariable or OpFunctionParam to find
    // the coherent and volatile decorations.
    while (BImagePtr->getOpCode() == OpAccessChain ||
           BImagePtr->getOpCode() == OpInBoundsAccessChain) {
      std::vector<SPIRVValue *> Operands =
          static_cast<SPIRVInstTemplateBase *>(BImagePtr)->getOperands();
      for (SPIRVValue *Operand : Operands) {
        if (Operand->hasDecorate(DecorationNonUniformEXT))
          Info->Flags |= Llpc::Builder::ImageFlagNonUniformImage;
      }
      BImagePtr = Operands[0];
    }
    assert(BImagePtr->getOpCode() == OpVariable ||
           BImagePtr->getOpCode() == OpFunctionParameter);
    if (BImageInst->hasDecorate(DecorationCoherent))
      Info->Flags |= Llpc::Builder::ImageFlagCoherent;
    if (BImageInst->hasDecorate(DecorationVolatile))
      Info->Flags |= Llpc::Builder::ImageFlagVolatile;
    return;
  }

  // We need to scan back through OpImage/OpSampledImage just to find any
  // NonUniform decoration.
  SPIRVValue *ScanBackInst = BImageInst;
  while (ScanBackInst->getOpCode() == OpImage ||
         ScanBackInst->getOpCode() == OpSampledImage) {
    if (ScanBackInst->getOpCode() == OpSampledImage) {
      auto Sampler =
          static_cast<SPIRVInstTemplateBase *>(ScanBackInst)->getOpValue(1);
      if (Sampler->hasDecorate(DecorationNonUniformEXT))
        Info->Flags |= Llpc::Builder::ImageFlagNonUniformSampler;
    }
    ScanBackInst =
        static_cast<SPIRVInstTemplateBase *>(ScanBackInst)->getOpValue(0);
    if (ScanBackInst->hasDecorate(DecorationNonUniformEXT))
      Info->Flags |= Llpc::Builder::ImageFlagNonUniformImage;
  }

  // Get the IR value for the image/sampledimage.
  Value *Desc =
      transValue(BImageInst, getBuilder()->GetInsertBlock()->getParent(),
                 getBuilder()->GetInsertBlock());

  SPIRVType *BImageTy = BImageInst->getType();
  if (BImageTy->getOpCode() == OpTypeSampledImage) {
    // For a sampledimage, the IR value is a struct containing the image and the
    // sampler.
    Info->SamplerDesc = getBuilder()->CreateExtractValue(Desc, 1);
    Desc = getBuilder()->CreateExtractValue(Desc, uint64_t(0));
    BImageTy = static_cast<SPIRVTypeSampledImage *>(BImageTy)->getImageType();
  }
  assert(BImageTy->getOpCode() == OpTypeImage);
  Info->Desc = &static_cast<const SPIRVTypeImage *>(BImageTy)->getDescriptor();
  Info->Dim = convertDimension(Info->Desc);

  if (Info->Desc->MS) {
    // For a multisampled image, the IR value is a struct containing the image
    // descriptor and the fmask descriptor.
    Info->FmaskDesc = getBuilder()->CreateExtractValue(Desc, 1);
    Desc = getBuilder()->CreateExtractValue(Desc, uint64_t(0));
  }

  Info->ImageDesc = Desc;
}

// =============================================================================
// Set up address operand array for image sample/gather/fetch/read/write
// builder call.
//
// \param BI The SPIR-V instruction
// \param MaskIdx Operand number of mask operand
// \param HasProj Whether there is an extra projective component in coordinate
// \param Addr [in/out] image address array
// \param ImageInfo [in/out] Decoded image type information; flags modified by
//        memory model image operands
// \param Sample [out] Where to store sample number for OpImageFetch; nullptr to
//        ignore it
void SPIRVToLLVM::setupImageAddressOperands(SPIRVInstruction *BI,
                                            unsigned MaskIdx, bool HasProj,
                                            MutableArrayRef<Value *> Addr,
                                            ExtractedImageInfo *ImageInfo,
                                            Value **SampleNum) {

  // SPIR-V allows the coordinate vector to be too wide; chop it down here.
  // Also handle the extra projective component if any.
  Value *Coord = Addr[Llpc::Builder::ImageAddressIdxCoordinate];
  if (auto CoordVecTy = dyn_cast<VectorType>(Coord->getType())) {
    unsigned NumCoords = getBuilder()->GetImageNumCoords(ImageInfo->Dim);
    if (HasProj) {
      Addr[Llpc::Builder::ImageAddressIdxProjective] =
          getBuilder()->CreateExtractElement(Coord, NumCoords);
    }
    if (NumCoords < CoordVecTy->getNumElements()) {
      static const unsigned Indexes[] = {0, 1, 2, 3};
      Coord = getBuilder()->CreateShuffleVector(
          Coord, Coord, ArrayRef<unsigned>(Indexes).slice(0, NumCoords));
      Addr[Llpc::Builder::ImageAddressIdxCoordinate] = Coord;
    }
  }

  // Extra image operands. These need to be in ascending order so they take
  // their operands in the right order.
  BasicBlock *BB = getBuilder()->GetInsertBlock();
  ArrayRef<SPIRVWord> ImageOpnds =
      ArrayRef<SPIRVWord>(
          static_cast<SPIRVInstTemplateBase *>(BI)->getOpWords())
          .slice(MaskIdx);
  if (!ImageOpnds.empty()) {
    unsigned Mask = ImageOpnds[0];
    ImageOpnds = ImageOpnds.slice(1);

    // Bias (0x1)
    if (Mask & ImageOperandsBiasMask) {
      Mask &= ~ImageOperandsBiasMask;
      Addr[Llpc::Builder::ImageAddressIdxLodBias] =
          transValue(BM->getValue(ImageOpnds[0]), BB->getParent(), BB);
      ImageOpnds = ImageOpnds.slice(1);
    }

    // Lod (0x2)
    if (Mask & ImageOperandsLodMask) {
      Mask &= ~ImageOperandsLodMask;
      Addr[Llpc::Builder::ImageAddressIdxLod] =
          transValue(BM->getValue(ImageOpnds[0]), BB->getParent(), BB);
      ImageOpnds = ImageOpnds.slice(1);
    }

    // Grad (0x4)
    if (Mask & ImageOperandsGradMask) {
      Mask &= ~ImageOperandsGradMask;
      Addr[Llpc::Builder::ImageAddressIdxDerivativeX] =
          transValue(BM->getValue(ImageOpnds[0]), BB->getParent(), BB);
      Addr[Llpc::Builder::ImageAddressIdxDerivativeY] =
          transValue(BM->getValue(ImageOpnds[1]), BB->getParent(), BB);
      ImageOpnds = ImageOpnds.slice(2);
    }

    // ConstOffset (0x8)
    if (Mask & ImageOperandsConstOffsetMask) {
      Mask &= ~ImageOperandsConstOffsetMask;
      Addr[Llpc::Builder::ImageAddressIdxOffset] =
          transValue(BM->getValue(ImageOpnds[0]), BB->getParent(), BB);
      ImageOpnds = ImageOpnds.slice(1);
    }

    // Offset (0x10)
    if (Mask & ImageOperandsOffsetMask) {
      Mask &= ~ImageOperandsOffsetMask;
      assert(!Addr[Llpc::Builder::ImageAddressIdxOffset]);
      Addr[Llpc::Builder::ImageAddressIdxOffset] =
          transValue(BM->getValue(ImageOpnds[0]), BB->getParent(), BB);
      ImageOpnds = ImageOpnds.slice(1);
    }

    // ConstOffsets (0x20)
    if (Mask & ImageOperandsConstOffsetsMask) {
      Mask &= ~ImageOperandsConstOffsetsMask;
      assert(!Addr[Llpc::Builder::ImageAddressIdxOffset]);
      Addr[Llpc::Builder::ImageAddressIdxOffset] =
          transValue(BM->getValue(ImageOpnds[0]), BB->getParent(), BB);
      ImageOpnds = ImageOpnds.slice(1);
    }

    // Sample (0x40) (only on OpImageFetch)
    if (Mask & ImageOperandsSampleMask) {
      Mask &= ~ImageOperandsSampleMask;
      if (SampleNum) {
        *SampleNum =
            transValue(BM->getValue(ImageOpnds[0]), BB->getParent(), BB);
      }
      ImageOpnds = ImageOpnds.slice(1);
    }

    // MinLod (0x80)
    if (Mask & ImageOperandsMinLodMask) {
      Mask &= ~ImageOperandsMinLodMask;
      Addr[Llpc::Builder::ImageAddressIdxLodClamp] =
          transValue(BM->getValue(ImageOpnds[0]), BB->getParent(), BB);
      ImageOpnds = ImageOpnds.slice(1);
    }

    // MakeTexelAvailableKHR (0x100)
    if (Mask & ImageOperandsMakeTexelAvailableKHRMask) {
      Mask &= ~ImageOperandsMakeTexelAvailableKHRMask;
      ImageInfo->Flags |= Llpc::Builder::ImageFlagCoherent;
    }

    // MakeTexelVisibleKHR (0x200)
    if (Mask & ImageOperandsMakeTexelVisibleKHRMask) {
      Mask &= ~ImageOperandsMakeTexelVisibleKHRMask;
      ImageInfo->Flags |= Llpc::Builder::ImageFlagCoherent;
    }

    // NonPrivateTexelKHR (0x400)
    if (Mask & ImageOperandsNonPrivateTexelKHRMask) {
      Mask &= ~ImageOperandsNonPrivateTexelKHRMask;
    }

    // VolatileTexelKHR (0x800)
    if (Mask & ImageOperandsVolatileTexelKHRMask) {
      Mask &= ~ImageOperandsVolatileTexelKHRMask;
      ImageInfo->Flags |= Llpc::Builder::ImageFlagVolatile;
    }

#if SPV_VERSION >= 0x10400
    // SignExtend (0x1000)
    if (Mask & ImageOperandsSignExtendMask) {
      Mask &= ~ImageOperandsSignExtendMask;
      ImageInfo->Flags |= Llpc::Builder::ImageFlagSignedResult;
    }

    // ZeroExtend (0x2000)
    if (Mask & ImageOperandsZeroExtendMask) {
      Mask &= ~ImageOperandsZeroExtendMask;
    }
#endif

    assert(!Mask && "Unknown image operand");
  }
}

// =============================================================================
// Handle fetch/read/write/atomic aspects of coordinate.
// This handles:
// 1. adding any offset onto the coordinate;
// 2. modifying coordinate for subpass data;
// 3. for a cube array, separating the layer and face, as expected by the
//    Builder interface
void SPIRVToLLVM::handleImageFetchReadWriteCoord(SPIRVInstruction *BI,
                                                 ExtractedImageInfo *ImageInfo,
                                                 MutableArrayRef<Value *> Addr,
                                                 bool EnableMultiView) {

  // Add the offset (if any) onto the coordinate. The offset might be narrower
  // than the coordinate.
  Value *Coord = Addr[Llpc::Builder::ImageAddressIdxCoordinate];
  if (auto Offset = Addr[Llpc::Builder::ImageAddressIdxOffset]) {
    if (isa<VectorType>(Coord->getType())) {
      if (!isa<VectorType>(Offset->getType())) {
        Offset = getBuilder()->CreateInsertElement(
            Constant::getNullValue(Coord->getType()), Offset, uint64_t(0));
      } else if (Coord->getType()->getVectorNumElements() !=
                 Offset->getType()->getVectorNumElements()) {
        Offset = getBuilder()->CreateShuffleVector(
            Offset, Constant::getNullValue(Offset->getType()),
            ArrayRef<unsigned>({0, 1, 2, 3})
                .slice(0, Coord->getType()->getVectorNumElements()));
      }
    }
    Coord = getBuilder()->CreateAdd(Coord, Offset);
  }

  if (ImageInfo->Desc->Dim == DimSubpassData) {
    // Modify coordinate for subpass data.
    if (!EnableMultiView) {
      // Subpass data without multiview: Add the x,y dimensions (converted to
      // signed int) of the fragment coordinate on to the texel coordate.
      ImageInfo->Flags |= Llpc::Builder::ImageFlagAddFragCoord;
    } else {
      // Subpass data with multiview: Use the fragment coordinate as x,y, and
      // use ViewIndex as z. We need to pass in a (0,0,0) coordinate.
      ImageInfo->Flags |= Llpc::Builder::ImageFlagAddFragCoord |
                          Llpc::Builder::ImageFlagCheckMultiView;
    }
  }

  // For a cube array, separate the layer and face.
  if (ImageInfo->Dim == Llpc::Builder::DimCubeArray) {
    SmallVector<Value *, 4> Components;
    for (unsigned I = 0; I != 3; ++I)
      Components.push_back(getBuilder()->CreateExtractElement(Coord, I));
    Components.push_back(
        getBuilder()->CreateUDiv(Components[2], getBuilder()->getInt32(6)));
    Components[2] =
        getBuilder()->CreateURem(Components[2], getBuilder()->getInt32(6));
    Coord = UndefValue::get(VectorType::get(getBuilder()->getInt32Ty(), 4));
    for (unsigned I = 0; I != 4; ++I)
      Coord = getBuilder()->CreateInsertElement(Coord, Components[I], I);
  }

  Addr[Llpc::Builder::ImageAddressIdxCoordinate] = Coord;
  return;
}

// =============================================================================
// Translate OpFragmentFetchAMD to LLVM IR
Value *SPIRVToLLVM::transSPIRVFragmentFetchFromInst(SPIRVInstruction *BI,
                                                    BasicBlock *BB) {

  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVInstTemplateBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  assert(ImageInfo.Desc->Dim == Dim2D || ImageInfo.Desc->Dim == DimSubpassData);
  ImageInfo.Dim = !ImageInfo.Desc->Arrayed ? Llpc::Builder::Dim2DMsaa
                                           : Llpc::Builder::Dim2DArrayMsaa;

  // Set up address arguments.
  Value *Coord = transValue(BII->getOpValue(1), BB->getParent(), BB);

  // Handle fetch/read/write/atomic aspects of coordinate. (This converts to
  // signed i32 and adds on the FragCoord if DimSubpassData.)
  Value *Addr[Llpc::Builder::ImageAddressCount] = {};
  Addr[Llpc::Builder::ImageAddressIdxCoordinate] = Coord;
  handleImageFetchReadWriteCoord(BI, &ImageInfo, Addr,
                                 /*EnableMultiView=*/false);
  Coord = Addr[Llpc::Builder::ImageAddressIdxCoordinate];

  // For a fragment fetch, there is an extra operand for the fragment id, which
  // we must supply as an extra coordinate.
  Value *FragId = transValue(BII->getOpValue(2), BB->getParent(), BB);
  Value *NewCoord = UndefValue::get(
      VectorType::get(getBuilder()->getInt32Ty(), 3 + ImageInfo.Desc->Arrayed));
  for (unsigned i = 0; i != 2 + ImageInfo.Desc->Arrayed; ++i) {
    NewCoord = getBuilder()->CreateInsertElement(
        NewCoord, getBuilder()->CreateExtractElement(Coord, i), i);
  }
  Coord = getBuilder()->CreateInsertElement(NewCoord, FragId,
                                          2 + ImageInfo.Desc->Arrayed);

  // Get the return type for the Builder method.
  Type *ResultTy = transType(BII->getType());

  // Create the image load.
  return getBuilder()->CreateImageLoad(ResultTy, ImageInfo.Dim, ImageInfo.Flags,
                                     ImageInfo.ImageDesc, Coord, nullptr);
}

// =============================================================================
// Translate OpFragmentMaskFetchAMD to LLVM IR
Value *SPIRVToLLVM::transSPIRVFragmentMaskFetchFromInst(SPIRVInstruction *BI,
                                                        BasicBlock *BB) {

  // Get image type descriptor and fmask descriptor.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVInstTemplateBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  assert(ImageInfo.Desc->Dim == Dim2D || ImageInfo.Desc->Dim == DimSubpassData);
  ImageInfo.Dim =
      !ImageInfo.Desc->Arrayed ? Llpc::Builder::Dim2D : Llpc::Builder::Dim3D;

  // Set up address arguments.
  Value *Coord = transValue(BII->getOpValue(1), BB->getParent(), BB);

  // Handle fetch/read/write/atomic aspects of coordinate. (This converts to
  // signed i32 and adds on the FragCoord if DimSubpassData.)
  Value *Addr[Llpc::Builder::ImageAddressCount] = {};
  Addr[Llpc::Builder::ImageAddressIdxCoordinate] = Coord;
  handleImageFetchReadWriteCoord(BI, &ImageInfo, Addr,
                                 /*EnableMultiView=*/false);
  Coord = Addr[Llpc::Builder::ImageAddressIdxCoordinate];

  // Get the return type for the Builder method. It returns v4f32, then we
  // extract just the R channel.
  Type *ResultTy = VectorType::get(transType(BI->getType()), 4);

  // Create the image load.
  Value *Result =
      getBuilder()->CreateImageLoad(ResultTy, ImageInfo.Dim, ImageInfo.Flags,
                                  ImageInfo.FmaskDesc, Coord, nullptr);
  return getBuilder()->CreateExtractElement(Result, uint64_t(0));
}

// =============================================================================
// Translate SPIR-V image atomic operations to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageAtomicOpFromInst(SPIRVInstruction *BI,
                                                    BasicBlock *BB) {
  // Parse the operands.
  unsigned OpndIdx = 0;
  auto BIT = static_cast<SPIRVInstTemplateBase *>(BI);
  auto PointerBI =
      static_cast<SPIRVImageTexelPointer *>(BIT->getOpValue(OpndIdx++));
  assert(PointerBI->getOpCode() == OpImageTexelPointer);
  unsigned Scope = static_cast<SPIRVConstant *>(BIT->getOpValue(OpndIdx++))
                       ->getZExtIntValue();
  unsigned Semantics = static_cast<SPIRVConstant *>(BIT->getOpValue(OpndIdx++))
                           ->getZExtIntValue();
  if (BIT->getOpCode() == OpAtomicCompareExchange) {
    // Ignore unequal memory semantics
    ++OpndIdx;
  }
  Value *InputData = nullptr;
  if (BIT->getOpCode() != OpAtomicLoad &&
      BIT->getOpCode() != OpAtomicIIncrement &&
      BIT->getOpCode() != OpAtomicIDecrement)
    InputData = transValue(BIT->getOpValue(OpndIdx++), BB->getParent(), BB);
  Value *Comparator = nullptr;
  if (BIT->getOpCode() == OpAtomicCompareExchange)
    Comparator = transValue(BIT->getOpValue(OpndIdx++), BB->getParent(), BB);

  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo ImageInfo = {BB};
  getImageDesc(PointerBI, &ImageInfo);

  // Set up address arguments.
  Value *Coord = transValue(PointerBI->getCoordinate(), BB->getParent(), BB);
  Value *SampleNum = transValue(PointerBI->getSample(), BB->getParent(), BB);

  // For a multi-sampled image, put the sample ID on the end.
  if (ImageInfo.Desc->MS) {
    SampleNum = getBuilder()->CreateInsertElement(
        UndefValue::get(Coord->getType()), SampleNum, uint64_t(0));
    SmallVector<unsigned, 4> Idxs;
    Idxs.push_back(0);
    Idxs.push_back(1);
    if (ImageInfo.Desc->Arrayed)
      Idxs.push_back(2);
    Idxs.push_back(Coord->getType()->getVectorNumElements());
    Coord = getBuilder()->CreateShuffleVector(Coord, SampleNum, Idxs);
  }

  // Handle fetch/read/write/atomic aspects of coordinate. (This separates the
  // cube face and ID.)
  Value *Addr[Llpc::Builder::ImageAddressCount] = {};
  Addr[Llpc::Builder::ImageAddressIdxCoordinate] = Coord;
  handleImageFetchReadWriteCoord(BI, &ImageInfo, Addr);
  Coord = Addr[Llpc::Builder::ImageAddressIdxCoordinate];

  // Determine the atomic ordering.
  AtomicOrdering Ordering = AtomicOrdering::NotAtomic;
  if (Scope != ScopeInvocation) {
    if (Semantics & MemorySemanticsSequentiallyConsistentMask)
      Ordering = AtomicOrdering::SequentiallyConsistent;
    else if (Semantics & MemorySemanticsAcquireReleaseMask)
      Ordering = AtomicOrdering::AcquireRelease;
    else if (Semantics & MemorySemanticsAcquireMask)
      Ordering = AtomicOrdering::Acquire;
    else if (Semantics & MemorySemanticsReleaseMask)
      Ordering = AtomicOrdering::Release;

    if (Ordering != AtomicOrdering::NotAtomic) {
      // Upgrade the ordering if we need to make it avaiable or visible
      if (Semantics & (MemorySemanticsMakeAvailableKHRMask |
                       MemorySemanticsMakeVisibleKHRMask))
        Ordering = AtomicOrdering::SequentiallyConsistent;
    }
  }

  // Create the image atomic op.
  unsigned AtomicOp = 0;
  Value *Result = nullptr;
  switch (BI->getOpCode()) {
  case OpAtomicCompareExchange:
    Result = getBuilder()->CreateImageAtomicCompareSwap(
        ImageInfo.Dim, ImageInfo.Flags, Ordering, ImageInfo.ImageDesc, Coord,
        InputData, Comparator);
    break;

  case OpAtomicStore:
  case OpAtomicExchange:
    AtomicOp = Llpc::Builder::ImageAtomicSwap;
    break;
  case OpAtomicLoad:
    AtomicOp = Llpc::Builder::ImageAtomicAdd;
    InputData = getBuilder()->getIntN(BIT->getType()->getBitWidth(), 0);
    break;
  case OpAtomicIIncrement:
    AtomicOp = Llpc::Builder::ImageAtomicAdd;
    InputData = getBuilder()->getIntN(BIT->getType()->getBitWidth(), 1);
    break;
  case OpAtomicIDecrement:
    AtomicOp = Llpc::Builder::ImageAtomicSub;
    InputData = getBuilder()->getIntN(BIT->getType()->getBitWidth(), 1);
    break;
  case OpAtomicIAdd:
    AtomicOp = Llpc::Builder::ImageAtomicAdd;
    break;
  case OpAtomicISub:
    AtomicOp = Llpc::Builder::ImageAtomicSub;
    break;
  case OpAtomicSMin:
    AtomicOp = Llpc::Builder::ImageAtomicSMin;
    break;
  case OpAtomicUMin:
    AtomicOp = Llpc::Builder::ImageAtomicUMin;
    break;
  case OpAtomicSMax:
    AtomicOp = Llpc::Builder::ImageAtomicSMax;
    break;
  case OpAtomicUMax:
    AtomicOp = Llpc::Builder::ImageAtomicUMax;
    break;
  case OpAtomicAnd:
    AtomicOp = Llpc::Builder::ImageAtomicAnd;
    break;
  case OpAtomicOr:
    AtomicOp = Llpc::Builder::ImageAtomicOr;
    break;
  case OpAtomicXor:
    AtomicOp = Llpc::Builder::ImageAtomicXor;
    break;

  default:
    llvm_unreachable("Unknown image atomic op");
    break;
  }

  if (!Result) {
    Result = getBuilder()->CreateImageAtomic(
        AtomicOp, ImageInfo.Dim, ImageInfo.Flags, Ordering, ImageInfo.ImageDesc,
        Coord, InputData);
  }
  return Result;
}

// =============================================================================
// Translate image sample to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageSampleFromInst(SPIRVInstruction *BI,
                                                  BasicBlock *BB) {
  // Get image type descriptor and load resource and sampler descriptors.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVImageInstBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  // Determine the return type we want from the builder call. For a sparse
  // sample/gather, the struct is {texel,TFE} in the builder call (to reflect
  // the hardware), but {TFE,texel} in SPIR-V.
  Type *OrigResultTy = transType(BII->getType());
  Type *ResultTy = OrigResultTy;
  if (auto StructResultTy = dyn_cast<StructType>(ResultTy)) {
    ResultTy = StructType::get(
        getBuilder()->getContext(),
        {StructResultTy->getElementType(1), StructResultTy->getElementType(0)});
  }

  // Set up address arguments.
  unsigned OpndIdx = 1;
  Value *Addr[Llpc::Builder::ImageAddressCount] = {};
  Addr[Llpc::Builder::ImageAddressIdxCoordinate] =
      transValue(BII->getOpValue(OpndIdx++), BB->getParent(), BB);

  switch (unsigned(BII->getOpCode())) {
  case OpImageSampleDrefImplicitLod:
  case OpImageSampleDrefExplicitLod:
  case OpImageSampleProjDrefImplicitLod:
  case OpImageSampleProjDrefExplicitLod:
  case OpImageSparseSampleDrefImplicitLod:
  case OpImageSparseSampleDrefExplicitLod:
  case OpImageSparseSampleProjDrefImplicitLod:
  case OpImageSparseSampleProjDrefExplicitLod:
    // This instruction has a dref operand.
    Addr[Llpc::Builder::ImageAddressIdxZCompare] =
        transValue(BII->getOpValue(OpndIdx++), BB->getParent(), BB);
    break;
  default:
    break;
  }

  bool HasProj = false;
  switch (BII->getOpCode()) {
  case OpImageSampleProjImplicitLod:
  case OpImageSampleProjExplicitLod:
  case OpImageSampleProjDrefImplicitLod:
  case OpImageSampleProjDrefExplicitLod:
  case OpImageSparseSampleProjImplicitLod:
  case OpImageSparseSampleProjExplicitLod:
  case OpImageSparseSampleProjDrefImplicitLod:
  case OpImageSparseSampleProjDrefExplicitLod:
    // This instruction has an extra projective coordinate component.
    HasProj = true;
    break;
  default:
    break;
  }

  setupImageAddressOperands(BII, OpndIdx, HasProj, Addr, &ImageInfo, nullptr);

  // Create the image sample call.
  Value *Result = getBuilder()->CreateImageSample(
      ResultTy, ImageInfo.Dim, ImageInfo.Flags, ImageInfo.ImageDesc,
      ImageInfo.SamplerDesc, Addr);

  // For a sparse sample, swap the struct elements back again.
  if (ResultTy != OrigResultTy) {
    Value *SwappedResult = getBuilder()->CreateInsertValue(
        UndefValue::get(OrigResultTy),
        getBuilder()->CreateExtractValue(Result, 1), unsigned(0));
    SwappedResult = getBuilder()->CreateInsertValue(
        SwappedResult, getBuilder()->CreateExtractValue(Result, unsigned(0)), 1);
    Result = SwappedResult;
  }
  return Result;
}

// =============================================================================
// Translate image gather to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageGatherFromInst(SPIRVInstruction *BI,
                                                  BasicBlock *BB) {
  // Get image type descriptor and load resource and sampler descriptors.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVImageInstBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  // Determine whether the result type of the gather is signed int.
  auto BIITy = BII->getType();
  if (BIITy->isTypeStruct())
    BIITy = static_cast<SPIRVTypeStruct *>(BIITy)->getMemberType(1);
  if (BIITy->isTypeVector())
    BIITy = static_cast<SPIRVTypeVector *>(BIITy)->getComponentType();
  if (BIITy->isTypeInt() && static_cast<SPIRVTypeInt *>(BIITy)->isSigned())
    ImageInfo.Flags |= Llpc::Builder::ImageFlagSignedResult;

  // Determine the return type we want from the builder call. For a sparse
  // sample/gather, the struct is {texel,TFE} in the builder call (to reflect
  // the hardware), but {TFE,texel} in SPIR-V.
  Type *OrigResultTy = transType(BII->getType());
  Type *ResultTy = OrigResultTy;
  if (auto StructResultTy = dyn_cast<StructType>(ResultTy)) {
    ResultTy = StructType::get(
        getBuilder()->getContext(),
        {StructResultTy->getElementType(1), StructResultTy->getElementType(0)});
  }

  // Set up address arguments.
  unsigned OpndIdx = 1;
  Value *Addr[Llpc::Builder::ImageAddressCount] = {};
  Addr[Llpc::Builder::ImageAddressIdxCoordinate] =
      transValue(BII->getOpValue(OpndIdx++), BB->getParent(), BB);

  switch (unsigned(BII->getOpCode())) {
  case OpImageGather:
  case OpImageSparseGather:
    // Component for OpImageGather
    Addr[Llpc::Builder::ImageAddressIdxComponent] =
        transValue(BII->getOpValue(OpndIdx++), BB->getParent(), BB);
    break;
  case OpImageDrefGather:
  case OpImageSparseDrefGather:
    // This instruction has a dref operand.
    Addr[Llpc::Builder::ImageAddressIdxZCompare] =
        transValue(BII->getOpValue(OpndIdx++), BB->getParent(), BB);
    break;
  default:
    break;
  }

  Value *ConstOffsets = nullptr;
  setupImageAddressOperands(BII, OpndIdx, /*HasProj=*/false, Addr, &ImageInfo,
                            nullptr);

  if (!Addr[Llpc::Builder::ImageAddressIdxLod] &&
      !Addr[Llpc::Builder::ImageAddressIdxLodBias] &&
      !Addr[Llpc::Builder::ImageAddressIdxDerivativeX]) {
    // A gather with no lod, bias or derivatives is done with lod 0, not
    // implicit lod. Except that does not happen if there is no lod clamp, and
    // this is a fragment shader, and CapabilityImageGatherBiasLodAMD was
    // declared.
    if (Addr[Llpc::Builder::ImageAddressIdxLodClamp] ||
        !EnableGatherLodNz) {
      Addr[Llpc::Builder::ImageAddressIdxLod] =
          Constant::getNullValue(getBuilder()->getFloatTy());
    }
  }

  Value *Result = nullptr;
  if (ConstOffsets) {
    // A gather with non-standard offsets is done as four separate gathers. If
    // it is a sparse gather, we just use the residency code from the last one.
    Result = UndefValue::get(ResultTy);
    Value *Residency = nullptr;
    if (ResultTy != OrigResultTy)
      Result = UndefValue::get(cast<StructType>(ResultTy)->getElementType(0));
    for (int Idx = 3; Idx >= 0; --Idx) {
      Addr[Llpc::Builder::ImageAddressIdxOffset] =
          getBuilder()->CreateExtractValue(ConstOffsets, Idx);
      Value *SingleResult = getBuilder()->CreateImageGather(
          ResultTy, ImageInfo.Dim, ImageInfo.Flags, ImageInfo.ImageDesc,
          ImageInfo.SamplerDesc, Addr);
      if (ResultTy != OrigResultTy) {
        // Handle sparse.
        Residency = getBuilder()->CreateExtractValue(SingleResult, 1);
        SingleResult = getBuilder()->CreateExtractValue(SingleResult, 0);
      }
      Result = getBuilder()->CreateInsertElement(
          Result, getBuilder()->CreateExtractElement(SingleResult, 3), Idx);
    }
    if (ResultTy != OrigResultTy) {
      // Handle sparse.
      Result = getBuilder()->CreateInsertValue(UndefValue::get(OrigResultTy),
                                             Result, 1);
      Result = getBuilder()->CreateInsertValue(Result, Residency, 0);
    }
    return Result;
  }

  // Create the image gather call.
  Result = getBuilder()->CreateImageGather(ResultTy, ImageInfo.Dim,
                                         ImageInfo.Flags, ImageInfo.ImageDesc,
                                         ImageInfo.SamplerDesc, Addr);

  // For a sparse gather, swap the struct elements back again.
  if (ResultTy != OrigResultTy) {
    Value *SwappedResult = getBuilder()->CreateInsertValue(
        UndefValue::get(OrigResultTy),
        getBuilder()->CreateExtractValue(Result, 1), unsigned(0));
    SwappedResult = getBuilder()->CreateInsertValue(
        SwappedResult, getBuilder()->CreateExtractValue(Result, unsigned(0)), 1);
    Result = SwappedResult;
  }
  return Result;
}

// =============================================================================
// Translate image fetch/read to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageFetchReadFromInst(SPIRVInstruction *BI,
                                                     BasicBlock *BB) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVImageInstBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  // Determine the return type we want from the builder call. For a sparse
  // fetch, the struct is {texel,TFE} in the builder call (to reflect
  // the hardware), but {TFE,texel} in SPIR-V.
  Type *OrigResultTy = transType(BI->getType());
  Type *ResultTy = OrigResultTy;
  if (auto StructResultTy = dyn_cast<StructType>(ResultTy)) {
    ResultTy = StructType::get(
        getBuilder()->getContext(),
        {StructResultTy->getElementType(1), StructResultTy->getElementType(0)});
  }

  // Set up address arguments.
  Value *Addr[Llpc::Builder::ImageAddressCount] = {};
  unsigned OpndIdx = 1;
  Addr[Llpc::Builder::ImageAddressIdxCoordinate] =
      transValue(BII->getOpValue(OpndIdx++), BB->getParent(), BB);

  Value *SampleNum = nullptr;
  setupImageAddressOperands(BII, OpndIdx, /*HasProj=*/false, Addr, &ImageInfo,
                            &SampleNum);

  // Handle fetch/read/write aspects of coordinate.
  handleImageFetchReadWriteCoord(BI, &ImageInfo, Addr);

  Value *Result = nullptr;
  Value *Coord = Addr[Llpc::Builder::ImageAddressIdxCoordinate];
  if (SampleNum) {
    if (BI->getOpCode() == OpImageFetch ||
        BI->getOpCode() == OpImageSparseFetch ||
        ImageInfo.Desc->Dim == DimSubpassData) {
      // This is an OpImageFetch with sample, or an OpImageRead with sample and
      // subpass data dimension. We need to use the fmask variant of the builder
      // method. First we need to get the fmask descriptor.
      Result = getBuilder()->CreateImageLoadWithFmask(
          ResultTy, ImageInfo.Dim, ImageInfo.Flags, ImageInfo.ImageDesc,
          ImageInfo.FmaskDesc, Coord, SampleNum);
    } else {
      // This is an OpImageRead with sample but not subpass data dimension.
      // Append the sample onto the coordinate.
      assert(ImageInfo.Dim == Llpc::Builder::Dim2DMsaa ||
             ImageInfo.Dim == Llpc::Builder::Dim2DArrayMsaa);
      SampleNum = getBuilder()->CreateInsertElement(
          UndefValue::get(Coord->getType()), SampleNum, uint64_t(0));
      Coord = getBuilder()->CreateShuffleVector(
          Coord, SampleNum,
          ArrayRef<unsigned>({0, 1, 2, 3})
              .slice(0,
                     cast<VectorType>(Coord->getType())->getNumElements() + 1));
    }
  }

  if (!Result) {
    // We did not do the "load with fmask" above. Do the normal image load now.
    Value *Lod = Addr[Llpc::Builder::ImageAddressIdxLod];
    Result =
        getBuilder()->CreateImageLoad(ResultTy, ImageInfo.Dim, ImageInfo.Flags,
                                    ImageInfo.ImageDesc, Coord, Lod);
  }

  // For a sparse read/fetch, swap the struct elements back again.
  if (ResultTy != OrigResultTy) {
    Value *SwappedResult = getBuilder()->CreateInsertValue(
        UndefValue::get(OrigResultTy),
        getBuilder()->CreateExtractValue(Result, 1), unsigned(0));
    SwappedResult = getBuilder()->CreateInsertValue(
        SwappedResult, getBuilder()->CreateExtractValue(Result, unsigned(0)), 1);
    Result = SwappedResult;
  }
  return Result;
}

// =============================================================================
// Translate image write to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageWriteFromInst(SPIRVInstruction *BI,
                                                 BasicBlock *BB) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVImageInstBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  // Set up address arguments and get the texel.
  Value *Addr[Llpc::Builder::ImageAddressCount] = {};
  unsigned OpndIdx = 1;
  Addr[Llpc::Builder::ImageAddressIdxCoordinate] =
      transValue(BII->getOpValue(OpndIdx++), BB->getParent(), BB);
  Value *Texel = transValue(BII->getOpValue(OpndIdx++), BB->getParent(), BB);

  Value *SampleNum = nullptr;
  setupImageAddressOperands(BII, OpndIdx, /*HasProj=*/false, Addr, &ImageInfo,
                            &SampleNum);

  // Handle fetch/read/write aspects of coordinate.
  handleImageFetchReadWriteCoord(BII, &ImageInfo, Addr);

  Value *Coord = Addr[Llpc::Builder::ImageAddressIdxCoordinate];
  if (SampleNum) {
    // Append the sample onto the coordinate.
    assert(ImageInfo.Dim == Llpc::Builder::Dim2DMsaa ||
           ImageInfo.Dim == Llpc::Builder::Dim2DArrayMsaa);
    SampleNum = getBuilder()->CreateInsertElement(
        UndefValue::get(Coord->getType()), SampleNum, uint64_t(0));
    Coord = getBuilder()->CreateShuffleVector(
        Coord, SampleNum,
        ArrayRef<unsigned>({0, 1, 2, 3})
            .slice(0,
                   cast<VectorType>(Coord->getType())->getNumElements() + 1));
  }

  // Do the image store.
  Value *Lod = Addr[Llpc::Builder::ImageAddressIdxLod];
  return getBuilder()->CreateImageStore(Texel, ImageInfo.Dim, ImageInfo.Flags,
                                      ImageInfo.ImageDesc, Coord, Lod);
}

// =============================================================================
// Translate OpImageQueryLevels to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageQueryLevelsFromInst(SPIRVInstruction *BI,
                                                       BasicBlock *BB) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVImageInstBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  // Generate the operation.
  return getBuilder()->CreateImageQueryLevels(ImageInfo.Dim, ImageInfo.Flags,
                                            ImageInfo.ImageDesc);
}

// =============================================================================
// Translate OpImageQuerySamples to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageQuerySamplesFromInst(SPIRVInstruction *BI,
                                                        BasicBlock *BB) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVImageInstBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  // Generate the operation.
  return getBuilder()->CreateImageQuerySamples(ImageInfo.Dim, ImageInfo.Flags,
                                             ImageInfo.ImageDesc);
}

// =============================================================================
// Translate OpImageQuerySize/OpImageQuerySizeLod to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageQuerySizeFromInst(SPIRVInstruction *BI,
                                                     BasicBlock *BB) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVImageInstBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  // Generate the operation.
  Value *Lod = getBuilder()->getInt32(0);
  if (BII->getOpCode() == OpImageQuerySizeLod)
    Lod = transValue(BII->getOpValue(1), BB->getParent(), BB);
  return getBuilder()->CreateImageQuerySize(ImageInfo.Dim, ImageInfo.Flags,
                                          ImageInfo.ImageDesc, Lod);
}

// =============================================================================
// Translate OpImageQueryLod to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageQueryLodFromInst(SPIRVInstruction *BI,
                                                    BasicBlock *BB) {
  // Get image type descriptor and load resource and sampler descriptors.
  ExtractedImageInfo ImageInfo = {BB};
  auto BII = static_cast<SPIRVImageInstBase *>(BI);
  getImageDesc(BII->getOpValue(0), &ImageInfo);

  // Generate the operation.
  Value *Coord = transValue(BII->getOpValue(1), BB->getParent(), BB);
  return getBuilder()->CreateImageGetLod(ImageInfo.Dim, ImageInfo.Flags,
                                       ImageInfo.ImageDesc,
                                       ImageInfo.SamplerDesc, Coord);
}

// =============================================================================

Instruction *SPIRVToLLVM::transSPIRVBuiltinFromInst(SPIRVInstruction *BI,
                                                    BasicBlock *BB) {
  assert(BB && "Invalid BB");
  return transBuiltinFromInst(getName(BI->getOpCode()), BI, BB);
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

  FpControlFlags.U32All = 0;
  static_assert(SPIRVTW_8Bit  == (8 >> 3),  "Unexpected value!");
  static_assert(SPIRVTW_16Bit == (16 >> 3), "Unexpected value!");
  static_assert(SPIRVTW_32Bit == (32 >> 3), "Unexpected value!");
  static_assert(SPIRVTW_64Bit == (64 >> 3), "Unexpected value!");

  if (auto EM = EntryTarget->getExecutionMode(ExecutionModeDenormPreserve))
      FpControlFlags.DenormPerserve = EM->getLiterals()[0] >> 3;

  if (auto EM = EntryTarget->getExecutionMode(ExecutionModeDenormFlushToZero))
      FpControlFlags.DenormFlushToZero = EM->getLiterals()[0] >> 3;

  if (auto EM = EntryTarget->getExecutionMode(ExecutionModeSignedZeroInfNanPreserve))
      FpControlFlags.SignedZeroInfNanPreserve = EM->getLiterals()[0] >> 3;

  if (auto EM = EntryTarget->getExecutionMode(ExecutionModeRoundingModeRTE))
      FpControlFlags.RoundingModeRTE = EM->getLiterals()[0] >> 3;

  if (auto EM = EntryTarget->getExecutionMode(ExecutionModeRoundingModeRTZ))
      FpControlFlags.RoundingModeRTZ = EM->getLiterals()[0] >> 3;

  // Set common shader mode (FP mode and useSubgroupSize) for middle-end.
  CommonShaderMode shaderMode = {};
  if (FpControlFlags.RoundingModeRTE & SPIRVTW_16Bit)
    shaderMode.fp16RoundMode = FpRoundMode::Even;
  else if (FpControlFlags.RoundingModeRTZ & SPIRVTW_16Bit)
    shaderMode.fp16RoundMode = FpRoundMode::Zero;
  if (FpControlFlags.RoundingModeRTE & SPIRVTW_32Bit)
    shaderMode.fp32RoundMode = FpRoundMode::Even;
  else if (FpControlFlags.RoundingModeRTZ & SPIRVTW_32Bit)
    shaderMode.fp32RoundMode = FpRoundMode::Zero;
  if (FpControlFlags.RoundingModeRTE & SPIRVTW_64Bit)
    shaderMode.fp64RoundMode = FpRoundMode::Even;
  else if (FpControlFlags.RoundingModeRTZ & SPIRVTW_64Bit)
    shaderMode.fp64RoundMode = FpRoundMode::Zero;
  if (FpControlFlags.DenormPerserve & SPIRVTW_16Bit)
    shaderMode.fp16DenormMode = FpDenormMode::FlushNone;
  else if (FpControlFlags.DenormFlushToZero & SPIRVTW_16Bit)
    shaderMode.fp16DenormMode = FpDenormMode::FlushInOut;
  if (FpControlFlags.DenormPerserve & SPIRVTW_32Bit)
    shaderMode.fp32DenormMode = FpDenormMode::FlushNone;
  else if (FpControlFlags.DenormFlushToZero & SPIRVTW_32Bit)
    shaderMode.fp32DenormMode = FpDenormMode::FlushInOut;
  if (FpControlFlags.DenormPerserve & SPIRVTW_64Bit)
    shaderMode.fp64DenormMode = FpDenormMode::FlushNone;
  else if (FpControlFlags.DenormFlushToZero & SPIRVTW_64Bit)
    shaderMode.fp64DenormMode = FpDenormMode::FlushInOut;

  auto &Extensions = BM->getExtension();
  if (Extensions.find("SPV_AMD_shader_ballot") != Extensions.end() ||
      BM->hasCapability(CapabilityGroupNonUniform) ||
      BM->hasCapability(CapabilityGroupNonUniformVote) ||
      BM->hasCapability(CapabilityGroupNonUniformArithmetic) ||
      BM->hasCapability(CapabilityGroupNonUniformBallot) ||
      BM->hasCapability(CapabilityGroupNonUniformShuffle) ||
      BM->hasCapability(CapabilityGroupNonUniformShuffleRelative) ||
      BM->hasCapability(CapabilityGroupNonUniformClustered) ||
      BM->hasCapability(CapabilityGroupNonUniformQuad) ||
      BM->hasCapability(CapabilitySubgroupBallotKHR) ||
      BM->hasCapability(CapabilitySubgroupVoteKHR) ||
      BM->hasCapability(CapabilityGroups))
    shaderMode.useSubgroupSize = true;

  getBuilder()->SetCommonShaderMode(shaderMode);

  EnableXfb = BM->getCapability().find(
      CapabilityTransformFeedback) != BM->getCapability().end();
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
        // NOTE: Constant folding is applied to OpSpecConstantOp because at this
        // time, specialization info is obtained and all specialization constants
        // get their own finalized specialization values.
        auto BI = static_cast<SPIRVSpecConstantOp *>(BV);
        BV = createValueFromSpecConstantOp(BI, FpControlFlags.RoundingModeRTE);
        BI->mapToConstant(BV);
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

  if (!transMetadata())
    return false;

  postProcessRowMajorMatrix();
  if (ModuleUsage->keepUnusedFunctions == false)
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
  case AddressingModelPhysicalStorageBuffer64EXT:
    break;
  default:
    SPIRVCKRT(0, InvalidAddressingModel,
              "Actual addressing mode is " +
                  std::to_string(BM->getAddressingModel()));
  }
  return true;
}

bool SPIRVToLLVM::transDecoration(SPIRVValue *BV, Value *V) {
  if (!transShaderDecoration(BV, V))
    return false;
  DbgTran.transDbgInfo(BV, V);
  return true;
}

bool SPIRVToLLVM::transNonTemporalMetadata(Instruction *I) {
  Constant *One = ConstantInt::get(Type::getInt32Ty(*Context), 1);
  MDNode *Node = MDNode::get(*Context, ConstantAsMetadata::get(One));
  I->setMetadata(M->getMDKindID("nontemporal"), Node);
  return true;
}

bool SPIRVToLLVM::transMetadata() {
  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    SPIRVFunction *BF = BM->getFunction(I);
    auto EntryPoint = BM->getEntryPoint(BF->getId());
    if (EntryPoint != nullptr && BF != EntryTarget)
      continue; // Ignore those untargeted entry-points

    if (EntryPoint == nullptr)
      continue;
    SPIRVExecutionModelKind ExecModel = EntryPoint->getExecModel();

    if ((ExecModel >= ExecutionModelVertex) && (ExecModel <= ExecutionModelGLCompute)) {

      // Generate metadata for execution modes
      ShaderExecModeMetadata ExecModeMD = {};
      ExecModeMD.common.FpControlFlags = FpControlFlags;

      if (ExecModel == ExecutionModelVertex) {
        if (BF->getExecutionMode(ExecutionModeXfb))
          ExecModeMD.vs.Xfb = true;

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
          ExecModeMD.ts.Xfb = true;

        if (auto EM = BF->getExecutionMode(ExecutionModeOutputVertices))
          ExecModeMD.ts.OutputVertices = EM->getLiterals()[0];

        // Give the tessellation mode to the middle-end.
        TessellationMode tessellationMode = {};
        tessellationMode.outputVertices = ExecModeMD.ts.OutputVertices;

        tessellationMode.vertexSpacing = VertexSpacing::Unknown;
        if (ExecModeMD.ts.SpacingEqual)
          tessellationMode.vertexSpacing = VertexSpacing::Equal;
        else if (ExecModeMD.ts.SpacingFractionalEven)
          tessellationMode.vertexSpacing = VertexSpacing::FractionalEven;
        else if (ExecModeMD.ts.SpacingFractionalOdd)
          tessellationMode.vertexSpacing = VertexSpacing::FractionalOdd;

        tessellationMode.vertexOrder = VertexOrder::Unknown;
        if (ExecModeMD.ts.VertexOrderCw)
          tessellationMode.vertexOrder = VertexOrder::Cw;
        else if (ExecModeMD.ts.VertexOrderCcw)
          tessellationMode.vertexOrder = VertexOrder::Ccw;

        tessellationMode.primitiveMode = PrimitiveMode::Unknown;
        if (ExecModeMD.ts.Triangles)
          tessellationMode.primitiveMode = PrimitiveMode::Triangles;
        else if (ExecModeMD.ts.Quads)
          tessellationMode.primitiveMode = PrimitiveMode::Quads;
        else if (ExecModeMD.ts.Isolines)
          tessellationMode.primitiveMode = PrimitiveMode::Isolines;

        tessellationMode.pointMode = false;
        if (ExecModeMD.ts.PointMode)
          tessellationMode.pointMode = true;

        getBuilder()->SetTessellationMode(tessellationMode);

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
          ExecModeMD.gs.Xfb = true;

        if (auto EM = BF->getExecutionMode(ExecutionModeInvocations))
          ExecModeMD.gs.Invocations = EM->getLiterals()[0];

        if (auto EM = BF->getExecutionMode(ExecutionModeOutputVertices))
          ExecModeMD.gs.OutputVertices = EM->getLiterals()[0];

        // Give the geometry mode to the middle-end.
        GeometryShaderMode geometryMode = {};
        geometryMode.invocations = 1;
        if (ExecModeMD.gs.Invocations > 0)
          geometryMode.invocations = ExecModeMD.gs.Invocations;
        geometryMode.outputVertices = ExecModeMD.gs.OutputVertices;

        if (ExecModeMD.gs.InputPoints)
          geometryMode.inputPrimitive = InputPrimitives::Points;
        else if (ExecModeMD.gs.InputLines)
          geometryMode.inputPrimitive = InputPrimitives::Lines;
        else if (ExecModeMD.gs.InputLinesAdjacency)
          geometryMode.inputPrimitive = InputPrimitives::LinesAdjacency;
        else if (ExecModeMD.gs.Triangles)
          geometryMode.inputPrimitive = InputPrimitives::Triangles;
        else if (ExecModeMD.gs.InputTrianglesAdjacency)
          geometryMode.inputPrimitive = InputPrimitives::TrianglesAdjacency;

        if (ExecModeMD.gs.OutputPoints)
          geometryMode.outputPrimitive = OutputPrimitives::Points;
        else if (ExecModeMD.gs.OutputLineStrip)
          geometryMode.outputPrimitive = OutputPrimitives::LineStrip;
        else if (ExecModeMD.gs.OutputTriangleStrip)
          geometryMode.outputPrimitive = OutputPrimitives::TriangleStrip;

        getBuilder()->SetGeometryShaderMode(geometryMode);

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

        if (BF->getExecutionMode(ExecutionModePostDepthCoverage))
          ExecModeMD.fs.PostDepthCoverage = true;

        // Give the fragment mode to the middle-end.
        FragmentShaderMode fragmentMode = {};
        fragmentMode.pixelCenterInteger = ExecModeMD.fs.PixelCenterInteger;
        fragmentMode.earlyFragmentTests = ExecModeMD.fs.EarlyFragmentTests;
        fragmentMode.postDepthCoverage  = ExecModeMD.fs.PostDepthCoverage;
        getBuilder()->SetFragmentShaderMode(fragmentMode);

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
            if (BuiltIn == spv::BuiltInWorkgroupSize) {
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

        // Give the workgroup size to the middle-end.
        ComputeShaderMode computeMode = {};
        computeMode.workgroupSizeX = ExecModeMD.cs.LocalSizeX;
        computeMode.workgroupSizeY = ExecModeMD.cs.LocalSizeY;
        computeMode.workgroupSizeZ = ExecModeMD.cs.LocalSizeZ;
        getBuilder()->SetComputeShaderMode(computeMode);
      } else
        llvm_unreachable("Invalid execution model");

      // Skip the following processing for GLSL
      continue;
    }
  }
  return true;
}

bool SPIRVToLLVM::checkContains64BitType(SPIRVType *BT) {
  if (BT->isTypeScalar())
    return (BT->getBitWidth() == 64);
  else if (BT->isTypeVector())
   return checkContains64BitType(BT->getVectorComponentType());
  else if (BT->isTypeMatrix())
   return checkContains64BitType(BT->getMatrixColumnType());
  else if (BT->isTypeArray())
   return checkContains64BitType(BT->getArrayElementType());
  else if (BT->isTypeStruct()) {
    bool Contains64BitType = false;
    auto MemberCount = BT->getStructMemberCount();
    for (auto MemberIdx = 0; MemberIdx < MemberCount; ++MemberIdx){
      auto  MemberTy = BT->getStructMemberType(MemberIdx);
      Contains64BitType = Contains64BitType || checkContains64BitType(MemberTy);
    }
    return Contains64BitType;
  } else {
    llvm_unreachable("Invalid type");
    return false;
  }
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
      InOutDec.IsXfb = false;
      InOutDec.XfbBuffer = 0;
      InOutDec.XfbStride = 0;
      InOutDec.XfbOffset = 0;
      InOutDec.contains64BitType = false;

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

      SPIRVWord XfbBuffer = SPIRVID_INVALID;
      if (BV->hasDecorate(DecorationXfbBuffer, 0, &XfbBuffer)) {
        InOutDec.IsXfb = true;
        InOutDec.XfbBuffer = XfbBuffer;
      }
      SPIRVWord XfbStride = SPIRVID_INVALID;
      if (BV->hasDecorate(DecorationXfbStride, 0, &XfbStride)) {
        InOutDec.IsXfb = true;
        InOutDec.XfbStride = XfbStride;
      }

      SPIRVWord XfbOffset = SPIRVID_INVALID;
      if (BV->hasDecorate(DecorationOffset, 0, &XfbOffset)) {
        // NOTE: Transform feedback is triggered only if "xfb_offset"
        // is specified.
        InOutDec.XfbOffset = XfbOffset;
      }

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

      BlockTy = BV->getType()->getPointerElementType();
      while (BlockTy->isTypeArray())
        BlockTy = BlockTy->getArrayElementType();
      bool IsStructTy = BlockTy->isTypeStruct();
      assert(IsStructTy);
      (void)IsStructTy;

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

      bool IsUniformBlock = false;

      if (BV->getType()->getPointerStorageClass() == StorageClassStorageBuffer)
        BlockType = BlockTypeShaderStorage;
      else {
        IsUniformBlock = BlockTy->hasDecorate(DecorationBlock);
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
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, BlockTy->getOpCode())));
      ResMDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, BlockType)));
      auto ResMDNode = MDNode::get(*Context, ResMDs);
      GV->addMetadata(gSPIRVMD::Resource, *ResMDNode);

      // Build block metadata
      ShaderBlockDecorate BlockDec = {};
      BlockDec.NonWritable = IsUniformBlock;
      Type *BlockMDTy = nullptr;
      auto BlockMD = buildShaderBlockMetadata(BlockTy, BlockDec, BlockMDTy);

      std::vector<Metadata*> BlockMDs;
      BlockMDs.push_back(ConstantAsMetadata::get(BlockMD));
      auto BlockMDNode = MDNode::get(*Context, BlockMDs);
      GV->addMetadata(gSPIRVMD::Block, *BlockMDNode);

    } else if (BV->getType()->isTypePointer() &&
        (BV->getType()->getPointerStorageClass() == StorageClassPushConstant)) {
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
      MDs.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, OpaqueTy->getOpCode())));
      auto MDNode = MDNode::get(*Context, MDs);
      GV->addMetadata(gSPIRVMD::Resource, *MDNode);

      // Build image memory metadata
      if (OpaqueTy->isTypeImage()) {
        auto ImageTy = static_cast<SPIRVTypeImage *>(OpaqueTy);
        auto Desc = ImageTy->getDescriptor();
        assert(Desc.Sampled <= 2); // 0 - runtime, 1 - sampled, 2 - non sampled

        if (Desc.Sampled == 2) {
          // For a storage image, build the memory metadata
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
  } else {
    bool IsNonUniform = BV->hasDecorate(DecorationNonUniformEXT);
    if (IsNonUniform && isa<Instruction>(V)) {
      std::vector<Value*> Args;
      Args.push_back(V);
      auto Types = getTypes(Args);
      auto VoidTy = Type::getVoidTy(*Context);
      auto BB = cast<Instruction>(V)->getParent();

      // Per-instruction metadata is not safe, LLVM optimizer may remove them,
      // so we choose to add a dummy instruction and remove them when it isn't
      // needed.
      std::string  MangledFuncName(gSPIRVMD::NonUniform);
      appendTypeMangling(nullptr, Args, MangledFuncName);
      auto F = getOrCreateFunction(M, VoidTy, Types, MangledFuncName);
      CallInst::Create(F, Args, "", BB);
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
  } else if (BT->isTypeForwardPointer()) {
    // Forward pointers in shader blocks are always 64-bit.
    BlockSize += 8;
  } else
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

  SPIRVWord XfbBuffer = SPIRVID_INVALID;
  if (BT->hasDecorate(DecorationXfbBuffer, 0, &XfbBuffer))
      InOutDec.XfbBuffer = XfbBuffer;

  SPIRVWord XfbStride = SPIRVID_INVALID;
  if (BT->hasDecorate(DecorationXfbStride, 0, &XfbStride))
      InOutDec.XfbStride = XfbStride;

  if (BT->isTypeScalar() || BT->isTypeVector()) {
    // Hanlde scalar or vector type
    assert(InOutDec.Value.U32All != SPIRVID_INVALID);

    // Build metadata for the scala/vector
    ShaderInOutMetadata InOutMD = {};
    if (InOutDec.IsXfb)
      InOutMD.IsXfb = true;

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
    InOutMD.XfbBuffer = InOutDec.XfbBuffer;
    InOutMD.XfbStride = InOutDec.XfbStride;
    InOutMD.XfbOffset = InOutDec.XfbOffset;
    InOutMD.XfbExtraOffset = InOutDec.XfbExtraOffset;

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
      uint32_t Alignment = 32;
      uint32_t BaseStride = 4; // Strides in (BYTES)
      InOutDec.XfbExtraOffset +=
        (((Width + Alignment - 1) / Alignment) * BaseStride);
    }

    auto Int64Ty = Type::getInt64Ty(*Context);
    std::vector<Type *> MDTys;
    MDTys.push_back(Int64Ty);     // Content of "ShaderInOutMetadata.U64All[0]"
    MDTys.push_back(Int64Ty);     // Content of "ShaderInOutMetadata.U64All[1]"
    MDTy = StructType::get(*Context, MDTys);

    std::vector<Constant *> MDValues;
    MDValues.push_back(ConstantInt::get(Int64Ty, InOutMD.U64All[0]));
    MDValues.push_back(ConstantInt::get(Int64Ty, InOutMD.U64All[1]));

    return ConstantStruct::get(static_cast<StructType *>(MDTy), MDValues);

  } else if (BT->isTypeArray() || BT->isTypeMatrix()) {
    // Handle array or matrix type
    auto Int32Ty = Type::getInt32Ty(*Context);
    auto Int64Ty = Type::getInt64Ty(*Context);

    // Build element metadata
    auto ElemTy = BT->isTypeArray() ? BT->getArrayElementType() :
                                      BT->getMatrixColumnType();
    uint32_t NumElems = BT->isTypeArray() ? BT->getArrayLength() :
                                            BT->getMatrixColumnCount();

    uint32_t StartLoc = InOutDec.Value.Loc;

    bool AlignTo64Bit = checkContains64BitType(ElemTy);

    uint32_t StartXfbExtraOffset = InOutDec.XfbExtraOffset;
    // Align StartXfbExtraOffset to 64-bit (8 bytes)
    if (AlignTo64Bit)
      StartXfbExtraOffset = roundUpToMultiple(
        InOutDec.XfbOffset + InOutDec.XfbExtraOffset, 8u) - InOutDec.XfbOffset;

    Type *ElemMDTy = nullptr;
    auto ElemDec = InOutDec; // Inherit from parent
    ElemDec.XfbExtraOffset = StartXfbExtraOffset;
    auto ElemMD = buildShaderInOutMetadata(ElemTy, ElemDec, ElemMDTy);

    if (ElemDec.PerPatch)
      InOutDec.PerPatch = true; // Set "per-patch" flag

    InOutDec.IsBlockArray = ElemTy->hasDecorate(DecorationBlock) ||
                            ElemDec.IsBlockArray; // Multi-dimension array

    uint32_t Stride = ElemDec.Value.Loc - StartLoc;

    uint32_t XfbArrayStride = 0;
    if (InOutDec.IsBlockArray) {
      // NOTE: For block array, each block array element is handled within its
      // own capture buffer. The transform feedback array stride is the flatten
      // dimension of an array element.
      assert(ElemTy->isTypeArray() || ElemTy->isTypeStruct());
      XfbArrayStride = ElemTy->isTypeArray() ?
        ElemDec.XfbArrayStride * ElemTy->getArrayLength() : 1;
    } else {
      // NOTE: For other non-block arrays, the transform feedback array stride
      // is the occupied byte count of an array element.
      XfbArrayStride = ElemDec.XfbExtraOffset - StartXfbExtraOffset;

      // Align XfbArrayStride to 64-bit (8 bytes)
      if (AlignTo64Bit)
        XfbArrayStride = roundUpToMultiple(XfbArrayStride, 8u);

      // Update next location value
      if (!InOutDec.IsBuiltIn) {
        InOutDec.Value.Loc = StartLoc + Stride * NumElems;
        InOutDec.XfbExtraOffset =
          StartXfbExtraOffset + XfbArrayStride * NumElems;
      }
    }

    // Built metadata for the array/matrix
    std::vector<Type *> MDTys;
    MDTys.push_back(Int32Ty);     // Stride
    MDTys.push_back(ElemMDTy);    // Element MD type
    MDTys.push_back(Int64Ty);     // Content of "ShaderInOutMetadata.U64All[0]"
    MDTys.push_back(Int64Ty);     // Content of "ShaderInOutMetadata.U64All[1]"
    MDTy = StructType::get(*Context, MDTys);

    ShaderInOutMetadata InOutMD = {};
    if (InOutDec.IsXfb)
      InOutMD.IsXfb = true;
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
    InOutMD.IsBlockArray = InOutDec.IsBlockArray;
    InOutMD.XfbBuffer = InOutDec.XfbBuffer;
    InOutMD.XfbStride = InOutDec.XfbStride;
    InOutMD.XfbOffset = InOutDec.XfbOffset;
    InOutMD.XfbArrayStride = XfbArrayStride;
    InOutMD.XfbExtraOffset = StartXfbExtraOffset;

    std::vector<Constant *> MDValues;
    MDValues.push_back(ConstantInt::get(Int32Ty, Stride));
    MDValues.push_back(ElemMD);
    MDValues.push_back(ConstantInt::get(Int64Ty, InOutMD.U64All[0]));
    MDValues.push_back(ConstantInt::get(Int64Ty, InOutMD.U64All[1]));

    return ConstantStruct::get(static_cast<StructType *>(MDTy), MDValues);

  } else if (BT->isTypeStruct()) {
    // Handle structure type
    std::vector<Type *>     MemberMDTys;
    std::vector<Constant *> MemberMDValues;

    // Build metadata for each structure member
    uint32_t XfbExtraOffset = InOutDec.XfbExtraOffset;
    uint32_t StructXfbExtraOffset = 0;
    auto NumMembers = BT->getStructMemberCount();

    // Get Block starting transform feedback offset,
    SPIRVWord BlockXfbOffset = SPIRVID_INVALID;
    SPIRVWord XfbOffset = SPIRVID_INVALID;

    // Do iteration to find the minimum member transform feedback offset as
    // starting block transform feedback offset
    for (auto MemberIdx = 0; MemberIdx < NumMembers; ++MemberIdx)
      if (BT->hasMemberDecorate(MemberIdx, DecorationOffset, 0, &XfbOffset))
        if (XfbOffset < BlockXfbOffset)
          BlockXfbOffset = XfbOffset;

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
        MemberDec.Component = MemberComponent;

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

      auto  MemberTy = BT->getStructMemberType(MemberIdx);
      bool AlignTo64Bit = checkContains64BitType(MemberTy);
      if (BT->hasMemberDecorate(MemberIdx, DecorationOffset, 0, &XfbOffset)) {
        // For the structure member, if it has DecorationOffset,
        // Then use DecorationOffset as starting XfbExtraOffset
        MemberDec.XfbExtraOffset = XfbOffset - BlockXfbOffset;
        MemberDec.XfbOffset = BlockXfbOffset;
      } else {
        if (AlignTo64Bit)
          // Align next XfbExtraOffset to 64-bit (8 bytes)
          MemberDec.XfbExtraOffset = roundUpToMultiple(XfbExtraOffset, 8u);
        else
          MemberDec.XfbExtraOffset = XfbExtraOffset;
      }
      XfbExtraOffset = MemberDec.XfbExtraOffset;
      SPIRVWord MemberStreamId = SPIRVID_INVALID;
      if (BT->hasMemberDecorate(
            MemberIdx, DecorationStream, 0, &MemberStreamId))
        MemberDec.StreamId = MemberStreamId;
      Type *MemberMDTy = nullptr;
      auto  MemberMD   =
        buildShaderInOutMetadata(MemberTy, MemberDec, MemberMDTy);

      // Align next XfbExtraOffset to 64-bit (8 bytes)
      XfbExtraOffset = MemberDec.XfbExtraOffset;

      if (AlignTo64Bit)
        XfbExtraOffset = roundUpToMultiple(XfbExtraOffset, 8u);

      StructXfbExtraOffset = std::max(StructXfbExtraOffset, XfbExtraOffset);

      if (MemberDec.IsBuiltIn)
        InOutDec.IsBuiltIn = true; // Set "builtin" flag
      else
        InOutDec.Value.Loc = MemberDec.Value.Loc; // Update next location value

      if (MemberDec.PerPatch)
        InOutDec.PerPatch = true; // Set "per-patch" flag

      MemberMDTys.push_back(MemberMDTy);
      MemberMDValues.push_back(MemberMD);
    }

    InOutDec.XfbExtraOffset = StructXfbExtraOffset;
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
  if (BT->isTypeVector() || BT->isTypeScalar()) {
    // Handle scalar or vector type
    ShaderBlockMetadata BlockMD = {};
    BlockMD.offset       = BlockDec.Offset;
    BlockMD.IsMatrix     = false; // Scalar or vector, clear matrix flag
    BlockMD.IsRowMajor   = false;
    BlockMD.MatrixStride = BlockDec.MatrixStride;
    BlockMD.Restrict     = BlockDec.Restrict;
    BlockMD.Coherent     = BlockDec.Coherent;
    BlockMD.Volatile     = BlockDec.Volatile;
    BlockMD.NonWritable  = BlockDec.NonWritable;
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
    BlockMD.IsRowMajor    = false;
    BlockMD.MatrixStride  = BlockDec.MatrixStride;
    BlockMD.Restrict      = BlockDec.Restrict;
    BlockMD.Coherent      = BlockDec.Coherent;
    BlockMD.Volatile      = BlockDec.Volatile;
    BlockMD.NonWritable   = BlockDec.NonWritable;
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
      SPIRVWord MemberMatrixStride = 0;

      // Check member decorations
      auto MemberDec = BlockDec; // Inherit from parent

      const uint32_t RemappedIdx = lookupRemappedTypeElements(BT, MemberIdx);
      const DataLayout &DL = M->getDataLayout();
      Type *const Ty = transType(BT, 0, false, true, true);
      assert(Ty->isStructTy());
      const StructLayout *const SL = DL.getStructLayout(
        static_cast<StructType *>(Ty));

      // Workaround SPIR-V 1.0 bug where sometimes structs had illegal overlap
      // in their struct offsets.
      if ((BM->getSPIRVVersion() == SPV_VERSION_1_0) &&
          (RemappedIdx == UINT32_MAX)) {
        MemberDec.Offset = UINT32_MAX;
      } else {
        MemberDec.Offset = SL->getElementOffset(RemappedIdx);
      }

      if (BT->hasMemberDecorate(MemberIdx, DecorationMatrixStride,
                                0, &MemberMatrixStride))
        MemberDec.MatrixStride = MemberMatrixStride;

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

      // Build metadata for structure member
      auto MemberTy = BT->getStructMemberType(MemberIdx);
      Type *MemberMDTy = nullptr;
      auto MemberMeta = buildShaderBlockMetadata(MemberTy, MemberDec, MemberMDTy);

      if (RemappedIdx > MemberIdx) {
        MemberMDTys.push_back(Type::getInt32Ty(*Context));
        MemberMDValues.push_back(UndefValue::get(Type::getInt32Ty(*Context)));
      }

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
  } else if (BT->isTypeForwardPointer()) {
    ShaderBlockMetadata BlockMD = {};
    BlockMD.offset       = BlockDec.Offset;
    BlockMD.IsMatrix     = false; // Scalar or vector, clear matrix flag
    BlockMD.IsRowMajor   = false;
    BlockMD.MatrixStride = 0;
    BlockMD.Restrict     = BlockDec.Restrict;
    BlockMD.Coherent     = BlockDec.Coherent;
    BlockMD.Volatile     = BlockDec.Volatile;
    BlockMD.NonWritable  = BlockDec.NonWritable;
    BlockMD.NonReadable  = BlockDec.NonReadable;

    MDTy = Type::getInt64Ty(*Context);
    return ConstantInt::get(MDTy, BlockMD.U64All);
  }

  llvm_unreachable("Invalid type");
  return nullptr;
}

// =============================================================================
// Translate GLSL.std.450 extended instruction
Value *SPIRVToLLVM::transGLSLExtInst(SPIRVExtInst *ExtInst,
                                           BasicBlock *BB) {
  auto BArgs = ExtInst->getArguments();
  auto Args = transValue(ExtInst->getValues(BArgs), BB->getParent(), BB);
  switch (static_cast<GLSLExtOpKind>(ExtInst->getExtOp())) {

  case GLSLstd450Round:
  case GLSLstd450RoundEven:
    // Round to whole number
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::rint, Args[0]);

  case GLSLstd450Trunc:
    // Trunc to whole number
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::trunc, Args[0]);

  case GLSLstd450FAbs:
    // FP absolute value
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, Args[0]);

  case GLSLstd450SAbs:
    // Signed integer absolute value
    return getBuilder()->CreateSAbs(Args[0]);

  case GLSLstd450FSign:
    // Get sign of FP value
    return getBuilder()->CreateFSign(Args[0]);

  case GLSLstd450SSign:
    // Get sign of signed integer value
    return getBuilder()->CreateSSign(Args[0]);

  case GLSLstd450Floor:
    // Round down to whole number
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::floor, Args[0]);

  case GLSLstd450Ceil:
    // Round up to whole number
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::ceil, Args[0]);

  case GLSLstd450Fract:
    // Get fractional part
    return getBuilder()->CreateFract(Args[0]);

  case GLSLstd450Radians:
    // Convert from degrees to radians
    return getBuilder()->CreateFMul(
        Args[0], getBuilder()->GetPiOver180(Args[0]->getType()));

  case GLSLstd450Degrees:
    // Convert from radians to degrees
    return getBuilder()->CreateFMul(
        Args[0], getBuilder()->Get180OverPi(Args[0]->getType()));

  case GLSLstd450Sin:
    // sin operation
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::sin, Args[0]);

  case GLSLstd450Cos:
    // cos operation
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::cos, Args[0]);

  case GLSLstd450Tan:
    // tan operation
    return getBuilder()->CreateTan(Args[0]);

  case GLSLstd450Asin:
    // arcsin operation
    return getBuilder()->CreateASin(Args[0]);

  case GLSLstd450Acos:
    // arccos operation
    return getBuilder()->CreateACos(Args[0]);

  case GLSLstd450Atan:
    // arctan operation
    return getBuilder()->CreateATan(Args[0]);

  case GLSLstd450Sinh:
    // hyperbolic sin operation
    return getBuilder()->CreateSinh(Args[0]);

  case GLSLstd450Cosh:
    // hyperbolic cos operation
    return getBuilder()->CreateCosh(Args[0]);

  case GLSLstd450Tanh:
    // hyperbolic tan operation
    return getBuilder()->CreateTanh(Args[0]);

  case GLSLstd450Asinh:
    // hyperbolic arcsin operation
    return getBuilder()->CreateASinh(Args[0]);

  case GLSLstd450Acosh:
    // hyperbolic arccos operation
    return getBuilder()->CreateACosh(Args[0]);

  case GLSLstd450Atanh:
    // hyperbolic arctan operation
    return getBuilder()->CreateATanh(Args[0]);

  case GLSLstd450Atan2:
    // arctan operation with Y/X input
    return getBuilder()->CreateATan2(Args[0], Args[1]);

  case GLSLstd450Pow:
    // Power: x^y
    return getBuilder()->CreatePower(Args[0], Args[1]);

  case GLSLstd450Exp:
    // Exponent: e^x
    return getBuilder()->CreateExp(Args[0]);

  case GLSLstd450Log:
    // Natural logarithm: log(x)
    return getBuilder()->CreateLog(Args[0]);

  case GLSLstd450Exp2:
    // Base 2 exponent: 2^x
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::exp2, Args[0]);

  case GLSLstd450Log2:
    // Base 2 logarithm: log2(x)
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::log2, Args[0]);

  case GLSLstd450Sqrt:
    // Square root
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::sqrt, Args[0]);

  case GLSLstd450InverseSqrt: {
    // Inverse square root
    Value *Sqrt = getBuilder()->CreateUnaryIntrinsic(Intrinsic::sqrt, Args[0]);
    return getBuilder()->CreateFDiv(ConstantFP::get(Sqrt->getType(), 1.0),
                                    Sqrt);
  }

  case GLSLstd450Determinant:
    // Determinant of square matrix
    return getBuilder()->CreateDeterminant(Args[0]);

  case GLSLstd450MatrixInverse:
    // Inverse of square matrix
    return getBuilder()->CreateMatrixInverse(Args[0]);

  case GLSLstd450Modf: {
    // Split input into fractional and whole number parts.
    Value *WholeNum =
        getBuilder()->CreateUnaryIntrinsic(Intrinsic::trunc, Args[0]);
    Value *Fract = getBuilder()->CreateFSub(Args[0], WholeNum);
    getBuilder()->CreateStore(WholeNum, Args[1]);
    return Fract;
  }

  case GLSLstd450ModfStruct: {
    // Split input into fractional and whole number parts.
    Value *WholeNum =
        getBuilder()->CreateUnaryIntrinsic(Intrinsic::trunc, Args[0]);
    Value *Fract = getBuilder()->CreateFSub(Args[0], WholeNum);
    Value *Result = UndefValue::get(transType(ExtInst->getType()));
    Result = getBuilder()->CreateInsertValue(Result, Fract, 0);
    Result = getBuilder()->CreateInsertValue(Result, WholeNum, 1);
    return Result;
  }

  case GLSLstd450FMin:
  case GLSLstd450NMin: {
    // FMin: FP minimum (undefined result for NaN)
    // NMin: FP minimum (preserve NaN)
    FastMathFlags FMF = getBuilder()->getFastMathFlags();
    FMF.setNoNaNs(ExtInst->getExtOp() == GLSLstd450FMin);
    getBuilder()->setFastMathFlags(FMF);
    return getBuilder()->CreateFMin(Args[0], Args[1]);
  }

  case GLSLstd450UMin: {
    // Unsigned integer minimum
    Value *Cmp = getBuilder()->CreateICmpULT(Args[1], Args[0]);
    return getBuilder()->CreateSelect(Cmp, Args[1], Args[0]);
  }

  case GLSLstd450SMin: {
    // Signed integer minimum
    Value *Cmp = getBuilder()->CreateICmpSLT(Args[1], Args[0]);
    return getBuilder()->CreateSelect(Cmp, Args[1], Args[0]);
  }

  case GLSLstd450FMax:
  case GLSLstd450NMax: {
    // FMax: FP maximum (undefined result for NaN)
    // NMax: FP maximum (preserve NaN)
    FastMathFlags FMF = getBuilder()->getFastMathFlags();
    FMF.setNoNaNs(ExtInst->getExtOp() == GLSLstd450FMax);
    getBuilder()->setFastMathFlags(FMF);
    return getBuilder()->CreateFMax(Args[0], Args[1]);
  }

  case GLSLstd450UMax: {
    // Unsigned integer maximum
    Value *Cmp = getBuilder()->CreateICmpULT(Args[1], Args[0]);
    return getBuilder()->CreateSelect(Cmp, Args[0], Args[1]);
  }

  case GLSLstd450SMax: {
    // Signed integer maximum
    Value *Cmp = getBuilder()->CreateICmpSLT(Args[1], Args[0]);
    return getBuilder()->CreateSelect(Cmp, Args[0], Args[1]);
  }

  case GLSLstd450FClamp:
  case GLSLstd450NClamp: {
    // FClamp: FP clamp with undefined result if any input is NaN
    // NClamp: FP clamp with "avoid NaN" semantics
    FastMathFlags PreservedFMF = getBuilder()->getFastMathFlags();
    FastMathFlags ModifiedFMF = PreservedFMF;
    ModifiedFMF.setNoNaNs(ExtInst->getExtOp() == GLSLstd450FClamp);
    getBuilder()->setFastMathFlags(ModifiedFMF);
    Value *Result = getBuilder()->CreateFClamp(Args[0], Args[1], Args[2]);
    getBuilder()->setFastMathFlags(PreservedFMF);
    return Result;
  }

  case GLSLstd450UClamp: {
    // Unsigned integer clamp
    Value *Cmp = getBuilder()->CreateICmpUGT(Args[1], Args[0]);
    Value *Max1 = getBuilder()->CreateSelect(Cmp, Args[1], Args[0]);
    Cmp = getBuilder()->CreateICmpULT(Args[2], Max1);
    return getBuilder()->CreateSelect(Cmp, Args[2], Max1);
  }

  case GLSLstd450SClamp: {
    // Signed integer clamp
    Value *Cmp = getBuilder()->CreateICmpSGT(Args[1], Args[0]);
    Value *Max1 = getBuilder()->CreateSelect(Cmp, Args[1], Args[0]);
    Cmp = getBuilder()->CreateICmpSLT(Args[2], Max1);
    return getBuilder()->CreateSelect(Cmp, Args[2], Max1);
  }

  case GLSLstd450FMix: {
    // Linear blend
    return getBuilder()->CreateFMix(Args[0], Args[1], Args[2]);
  }

  case GLSLstd450Step: {
    // x < edge ? 0.0 : 1.0
    Value *Edge = Args[0];
    Value *X = Args[1];
    Value *Cmp = getBuilder()->CreateFCmpOLT(X, Edge);
    return getBuilder()->CreateSelect(Cmp, Constant::getNullValue(X->getType()),
                                      ConstantFP::get(X->getType(), 1.0));
  }

  case GLSLstd450SmoothStep:
    // Smooth step operation
    return getBuilder()->CreateSmoothStep(Args[0], Args[1], Args[2]);

  case GLSLstd450Fma:
    // Fused multiply and add
    return getBuilder()->CreateFma(Args[0], Args[1], Args[2]);

  case GLSLstd450Frexp:
  case GLSLstd450FrexpStruct: {
    // Split input into significand (mantissa) and exponent.
    Value *Mant = getBuilder()->CreateExtractSignificand(Args[0]);
    Value *Exp = getBuilder()->CreateExtractExponent(Args[0]);
    if (ExtInst->getExtOp() == GLSLstd450FrexpStruct) {
      // FrexpStruct: Return the two values as a struct.
      Value *Result = UndefValue::get(transType(ExtInst->getType()));
      Result = getBuilder()->CreateInsertValue(Result, Mant, 0);
      Exp = getBuilder()->CreateSExtOrTrunc(
          Exp, Result->getType()->getStructElementType(1));
      Result = getBuilder()->CreateInsertValue(Result, Exp, 1);
      return Result;
    }
    // Frexp: Store the exponent and return the mantissa.
    Exp = getBuilder()->CreateSExtOrTrunc(
        Exp, Args[1]->getType()->getPointerElementType());
    getBuilder()->CreateStore(Exp, Args[1]);
    return Mant;
  }

  case GLSLstd450Ldexp:
    // Construct FP value from mantissa and exponent
    return getBuilder()->CreateLdexp(Args[0], Args[1]);

  case GLSLstd450PackSnorm4x8: {
    // Convert <4 x float> into signed normalized <4 x i8> then pack into i32.
    Value *Val = getBuilder()->CreateFClamp(
        Args[0], ConstantFP::get(Args[0]->getType(), -1.0),
        ConstantFP::get(Args[0]->getType(), 1.0));
    Val = getBuilder()->CreateFMul(Val,
                                   ConstantFP::get(Args[0]->getType(), 127.0));
    Val = getBuilder()->CreateUnaryIntrinsic(Intrinsic::rint, Val);
    Val = getBuilder()->CreateFPToSI(
        Val, VectorType::get(getBuilder()->getInt8Ty(), 4));
    return getBuilder()->CreateBitCast(Val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackUnorm4x8: {
    // Convert <4 x float> into unsigned normalized <4 x i8> then pack into i32.
    Value *Val = getBuilder()->CreateFClamp(
        Args[0], Constant::getNullValue(Args[0]->getType()),
        ConstantFP::get(Args[0]->getType(), 1.0));
    Val = getBuilder()->CreateFMul(Val,
                                   ConstantFP::get(Args[0]->getType(), 255.0));
    Val = getBuilder()->CreateFPToUI(
        Val, VectorType::get(getBuilder()->getInt8Ty(), 4));
    return getBuilder()->CreateBitCast(Val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackSnorm2x16: {
    // Convert <2 x float> into signed normalized <2 x i16> then pack into i32.
    Value *Val = getBuilder()->CreateFClamp(
        Args[0], ConstantFP::get(Args[0]->getType(), -1.0),
        ConstantFP::get(Args[0]->getType(), 1.0));
    Val = getBuilder()->CreateFMul(
        Val, ConstantFP::get(Args[0]->getType(), 32767.0));
    Val = getBuilder()->CreateFPToSI(
        Val, VectorType::get(getBuilder()->getInt16Ty(), 2));
    return getBuilder()->CreateBitCast(Val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackUnorm2x16: {
    // Convert <2 x float> into unsigned normalized <2 x i16> then pack into
    // i32.
    Value *Val = getBuilder()->CreateFClamp(
        Args[0], Constant::getNullValue(Args[0]->getType()),
        ConstantFP::get(Args[0]->getType(), 1.0));
    Val = getBuilder()->CreateFMul(
        Val, ConstantFP::get(Args[0]->getType(), 65535.0));
    Val = getBuilder()->CreateFPToUI(
        Val, VectorType::get(getBuilder()->getInt16Ty(), 2));
    return getBuilder()->CreateBitCast(Val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackHalf2x16: {
    // Convert <2 x float> into <2 x half> then pack into i32.
    Value *Val = getBuilder()->CreateFPTrunc(
        Args[0], VectorType::get(getBuilder()->getHalfTy(), 2));
    return getBuilder()->CreateBitCast(Val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackDouble2x32:
    // Cast <2 x i32> to double.
    return getBuilder()->CreateBitCast(Args[0], getBuilder()->getDoubleTy());

  case GLSLstd450UnpackSnorm2x16: {
    // Unpack i32 into <2 x i16> then treat as signed normalized and convert to
    // <2 x float>.
    Value *Val = getBuilder()->CreateBitCast(
        Args[0], VectorType::get(getBuilder()->getInt16Ty(), 2));
    Val = getBuilder()->CreateSIToFP(
        Val, VectorType::get(getBuilder()->getFloatTy(), 2));
    Value *Multiplier =
        getBuilder()->GetOneOverPower2MinusOne(Val->getType(), 15); // 1/32767
    Val = getBuilder()->CreateFMul(Val, Multiplier);
    return getBuilder()->CreateFClamp(Val,
                                   ConstantFP::get(Val->getType(), -1.0),
                                   ConstantFP::get(Val->getType(), 1.0));
  }

  case GLSLstd450UnpackUnorm2x16: {
    // Unpack i32 into <2 x i16> then treat as unsigned normalized and convert
    // to <2 x float>.
    Value *Val = getBuilder()->CreateBitCast(
        Args[0], VectorType::get(getBuilder()->getInt16Ty(), 2));
    Val = getBuilder()->CreateUIToFP(
        Val, VectorType::get(getBuilder()->getFloatTy(), 2));
    Value *Multiplier =
        getBuilder()->GetOneOverPower2MinusOne(Val->getType(), 16); // 1/65535
    return getBuilder()->CreateFMul(Val, Multiplier);
  }

  case GLSLstd450UnpackHalf2x16: {
    // Unpack <2 x half> from i32 and convert to <2 x float>.
    // This is required to flush denorm to zero if that mode is enabled.
    Value *Val = getBuilder()->CreateBitCast(
        Args[0], VectorType::get(getBuilder()->getHalfTy(), 2));
    Val = flushDenorm(Val);
    return getBuilder()->CreateFPExt(
        Val, VectorType::get(getBuilder()->getFloatTy(), 2));
  }

  case GLSLstd450UnpackSnorm4x8: {
    // Unpack i32 into <4 x i8> then treat as signed normalized and convert to
    // <4 x float>.
    Value *Val = getBuilder()->CreateBitCast(
        Args[0], VectorType::get(getBuilder()->getInt8Ty(), 4));
    Val = getBuilder()->CreateSIToFP(
        Val, VectorType::get(getBuilder()->getFloatTy(), 4));
    Value *Multiplier =
        getBuilder()->GetOneOverPower2MinusOne(Val->getType(), 7); // 1/127
    Val = getBuilder()->CreateFMul(Val, Multiplier);
    return getBuilder()->CreateFClamp(Val,
                                   ConstantFP::get(Val->getType(), -1.0),
                                   ConstantFP::get(Val->getType(), 1.0));
  }

  case GLSLstd450UnpackUnorm4x8: {
    // Unpack i32 into <4 x i8> then treat as unsigned normalized and convert to
    // <4 x float>.
    Value *Val = getBuilder()->CreateBitCast(
        Args[0], VectorType::get(getBuilder()->getInt8Ty(), 4));
    Val = getBuilder()->CreateUIToFP(
        Val, VectorType::get(getBuilder()->getFloatTy(), 4));
    Value *Multiplier =
        getBuilder()->GetOneOverPower2MinusOne(Val->getType(), 8); // 1/255
    return getBuilder()->CreateFMul(Val, Multiplier);
  }

  case GLSLstd450UnpackDouble2x32:
    // Cast double to <2 x i32>.
    return getBuilder()->CreateBitCast(
        Args[0], VectorType::get(getBuilder()->getInt32Ty(), 2));

  case GLSLstd450Length: {
    // Get length of vector.
    if (!isa<VectorType>(Args[0]->getType())) {
      return getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, Args[0]);
    }
    Value *Dot = getBuilder()->CreateDotProduct(Args[0], Args[0]);
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::sqrt, Dot);
  }

  case GLSLstd450Distance: {
    // Get distance between two points.
    Value *Diff = getBuilder()->CreateFSub(Args[0], Args[1]);
    if (!isa<VectorType>(Diff->getType())) {
      return getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, Diff);
    }
    Value *Dot = getBuilder()->CreateDotProduct(Diff, Diff);
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::sqrt, Dot);
  }

  case GLSLstd450Cross:
    // Vector cross product.
    return getBuilder()->CreateCrossProduct(Args[0], Args[1]);

  case GLSLstd450Normalize:
    // Normalize vector to magnitude 1.
    return getBuilder()->CreateNormalizeVector(Args[0]);

  case GLSLstd450FaceForward:
    // Face forward operation.
    return getBuilder()->CreateFaceForward(Args[0], Args[1], Args[2]);

  case GLSLstd450Reflect:
    // Reflect operation.
    return getBuilder()->CreateReflect(Args[0], Args[1]);

  case GLSLstd450Refract:
    // Refract operation.
    return getBuilder()->CreateRefract(Args[0], Args[1], Args[2]);

  case GLSLstd450FindILsb: {
    // Find integer least-significant 1-bit. 0 input gives -1 result.
    // The spec claims that the result must be the same type as the input, but I
    // have seen SPIR-V that does not do that.
    Value *IsZero = getBuilder()->CreateICmpEQ(
        Args[0], Constant::getNullValue(Args[0]->getType()));
    Value *Result = getBuilder()->CreateBinaryIntrinsic(
        Intrinsic::cttz, Args[0], getBuilder()->getTrue());
    Result = getBuilder()->CreateSelect(
        IsZero, Constant::getAllOnesValue(Result->getType()), Result);
    return getBuilder()->CreateSExtOrTrunc(Result,
                                           transType(ExtInst->getType()));
  }

  case GLSLstd450FindSMsb: {
    // Find signed integer most-significant bit. 0 or -1 input gives -1 result.
    Value *Result = getBuilder()->CreateFindSMsb(Args[0]);
    // TODO: According to the SPIR-V spec, FindSMsb expects the input value and result to have both the
    // same number of components and the same component width. But glslang violates this rule. Thus,
    // we have a workaround here for this mismatch.
    return getBuilder()->CreateSExtOrTrunc(Result,
                                           transType(ExtInst->getType()));
  }

  case GLSLstd450FindUMsb: {
    // Find unsigned integer most-significant 1-bit. 0 input gives -1 result.
    // The spec claims that the result must be the same type as the input, but I
    // have seen SPIR-V that does not do that.
    Value *Result = getBuilder()->CreateBinaryIntrinsic(
        Intrinsic::ctlz, Args[0], getBuilder()->getFalse());
    Result = getBuilder()->CreateSub(
        ConstantInt::get(
            Result->getType(),
            Result->getType()->getScalarType()->getPrimitiveSizeInBits() - 1),
        Result);
    return getBuilder()->CreateSExtOrTrunc(Result,
                                           transType(ExtInst->getType()));
  }

  case GLSLstd450InterpolateAtCentroid:
  case GLSLstd450InterpolateAtSample:
  case GLSLstd450InterpolateAtOffset:
    // These InterpolateAt* instructions are handled the old way, by generating
    // a call.
    return transGLSLBuiltinFromExtInst(ExtInst, BB);

  default:
    llvm_unreachable("Unrecognized GLSLstd450 extended instruction");
  }
}

// =============================================================================
// Flush denorm to zero if DenormFlushToZero is set in the shader
Value *SPIRVToLLVM::flushDenorm(Value *Val) {
  if ((FpControlFlags.DenormFlushToZero * 8) &
      Val->getType()->getScalarType()->getPrimitiveSizeInBits())
    Val = getBuilder()->CreateUnaryIntrinsic(Intrinsic::canonicalize, Val);
  return Val;
}

// =============================================================================
// Translate ShaderTrinaryMinMax extended instructions
Value *SPIRVToLLVM::transTrinaryMinMaxExtInst(SPIRVExtInst *ExtInst,
                                              BasicBlock *BB) {
  auto BArgs = ExtInst->getArguments();
  auto Args = transValue(ExtInst->getValues(BArgs), BB->getParent(), BB);
  switch (ExtInst->getExtOp()) {

  case FMin3AMD: {
    // Minimum of three FP values. Undefined result if any NaNs.
    FastMathFlags FMF = getBuilder()->getFastMathFlags();
    FMF.setNoNaNs();
    getBuilder()->setFastMathFlags(FMF);
    return getBuilder()->CreateFMin3(Args[0], Args[1], Args[2]);
  }

  case FMax3AMD: {
    // Maximum of three FP values. Undefined result if any NaNs.
    FastMathFlags FMF = getBuilder()->getFastMathFlags();
    FMF.setNoNaNs();
    getBuilder()->setFastMathFlags(FMF);
    return getBuilder()->CreateFMax3(Args[0], Args[1], Args[2]);
  }

  case FMid3AMD: {
    // Middle of three FP values. Undefined result if any NaNs.
    FastMathFlags FMF = getBuilder()->getFastMathFlags();
    FMF.setNoNaNs();
    getBuilder()->setFastMathFlags(FMF);
    return getBuilder()->CreateFMid3(Args[0], Args[1], Args[2]);
  }

  case UMin3AMD: {
    // Minimum of three unsigned integer values.
    Value *Cond = getBuilder()->CreateICmpULT(Args[0], Args[1]);
    Value *Min1 = getBuilder()->CreateSelect(Cond, Args[0], Args[1]);
    Cond = getBuilder()->CreateICmpULT(Min1, Args[2]);
    return getBuilder()->CreateSelect(Cond, Min1, Args[2]);
  }

  case UMax3AMD: {
    // Maximum of three unsigned integer values.
    Value *Cond = getBuilder()->CreateICmpUGT(Args[0], Args[1]);
    Value *Max1 = getBuilder()->CreateSelect(Cond, Args[0], Args[1]);
    Cond = getBuilder()->CreateICmpUGT(Max1, Args[2]);
    return getBuilder()->CreateSelect(Cond, Max1, Args[2]);
  }

  case UMid3AMD: {
    // Middle of three unsigned integer values.
    Value *Cond = getBuilder()->CreateICmpULT(Args[0], Args[1]);
    Value *Min1 = getBuilder()->CreateSelect(Cond, Args[0], Args[1]);
    Cond = getBuilder()->CreateICmpUGT(Args[0], Args[1]);
    Value *Max1 = getBuilder()->CreateSelect(Cond, Args[0], Args[1]);
    Cond = getBuilder()->CreateICmpULT(Max1, Args[2]);
    Value *Min2 = getBuilder()->CreateSelect(Cond, Max1, Args[2]);
    Cond = getBuilder()->CreateICmpUGT(Min1, Min2);
    return getBuilder()->CreateSelect(Cond, Min1, Min2);
  }

  case SMin3AMD: {
    // Minimum of three signed integer values.
    Value *Cond = getBuilder()->CreateICmpSLT(Args[0], Args[1]);
    Value *Min1 = getBuilder()->CreateSelect(Cond, Args[0], Args[1]);
    Cond = getBuilder()->CreateICmpSLT(Min1, Args[2]);
    return getBuilder()->CreateSelect(Cond, Min1, Args[2]);
  }

  case SMax3AMD: {
    // Maximum of three signed integer values.
    Value *Cond = getBuilder()->CreateICmpSGT(Args[0], Args[1]);
    Value *Max1 = getBuilder()->CreateSelect(Cond, Args[0], Args[1]);
    Cond = getBuilder()->CreateICmpSGT(Max1, Args[2]);
    return getBuilder()->CreateSelect(Cond, Max1, Args[2]);
  }

  case SMid3AMD: {
    // Middle of three signed integer values.
    Value *Cond = getBuilder()->CreateICmpSLT(Args[0], Args[1]);
    Value *Min1 = getBuilder()->CreateSelect(Cond, Args[0], Args[1]);
    Cond = getBuilder()->CreateICmpSGT(Args[0], Args[1]);
    Value *Max1 = getBuilder()->CreateSelect(Cond, Args[0], Args[1]);
    Cond = getBuilder()->CreateICmpSLT(Max1, Args[2]);
    Value *Min2 = getBuilder()->CreateSelect(Cond, Max1, Args[2]);
    Cond = getBuilder()->CreateICmpSGT(Min1, Min2);
    return getBuilder()->CreateSelect(Cond, Min1, Min2);
  }

  default:
    llvm_unreachable("Unrecognized ShaderTrinaryMinMax instruction");
  }
}

// =============================================================================
Value *SPIRVToLLVM::transGLSLBuiltinFromExtInst(SPIRVExtInst *BC,
                                                BasicBlock *BB) {
  assert(BB && "Invalid BB");

  SPIRVExtInstSetKind Set = BM->getBuiltinSet(BC->getExtSetId());
  assert((Set == SPIRVEIS_GLSL ||
          Set == SPIRVEIS_ShaderExplicitVertexParameterAMD)
         && "Not valid extended instruction");

  SPIRVWord EntryPoint = BC->getExtOp();
  auto BArgs = BC->getArguments();
  std::vector<Type *> ArgTys = transTypeVector(BC->getValueTypes(BArgs));
  string UnmangledName = "";
  if (Set == SPIRVEIS_GLSL)
    UnmangledName =  GLSLExtOpMap::map(
      static_cast<GLSLExtOpKind>(EntryPoint));
  else if (Set == SPIRVEIS_ShaderExplicitVertexParameterAMD)
    UnmangledName = ShaderExplicitVertexParameterAMDExtOpMap::map(
      static_cast<ShaderExplicitVertexParameterAMDExtOpKind>(EntryPoint));

  string MangledName(UnmangledName);
  std::vector<Value *> Args = transValue(BC->getArgumentValues(),
                                         BB->getParent(),
                                         BB);
  appendTypeMangling(nullptr, Args, MangledName);
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
  CallInst *Call = CallInst::Create(Func,
                                    Args,
                                    BC->getName(),
                                    BB);
  setCallingConv(Call);
  addFnAttr(Context, Call, Attribute::NoUnwind);
  return Call;
}

Instruction *SPIRVToLLVM::transBarrier(BasicBlock *BB, SPIRVWord ExecScope,
                                          SPIRVWord MemSema,
                                          SPIRVWord MemScope) {
  transMemFence(BB, MemSema, MemScope);
  return getBuilder()->CreateBarrier();
}

Instruction *SPIRVToLLVM::transMemFence(BasicBlock *BB, SPIRVWord MemSema,
                                        SPIRVWord MemScope) {
  AtomicOrdering Ordering = AtomicOrdering::NotAtomic;

  if (MemSema & MemorySemanticsSequentiallyConsistentMask)
    Ordering = AtomicOrdering::SequentiallyConsistent;
  else if (MemSema & MemorySemanticsAcquireReleaseMask)
    Ordering = AtomicOrdering::AcquireRelease;
  else if (MemSema & MemorySemanticsAcquireMask)
    Ordering = AtomicOrdering::Acquire;
  else if (MemSema & MemorySemanticsReleaseMask)
    Ordering = AtomicOrdering::Release;
  else if (MemSema != MemorySemanticsMaskNone &&
           BM->getMemoryModel() != MemoryModelVulkan) {
    // Some shaders written for pre-Vulkan memory models use e.g.:
    // OpMemoryBarrier 1, 512 // 512 = CrossWorkgroupMemory
    // and expect some ordering, even though none of the low 4 (ordering) bits
    // of the semantics are set, so we set a reasonable default here.
    Ordering = AtomicOrdering::AcquireRelease;
  }

  if (Ordering == AtomicOrdering::NotAtomic)
    return nullptr;

  // Upgrade the ordering if we need to make it available or visible
  if (MemSema & (MemorySemanticsMakeAvailableKHRMask |
		 MemorySemanticsMakeVisibleKHRMask))
    Ordering = AtomicOrdering::SequentiallyConsistent;

  SyncScope::ID Scope = SyncScope::System;

  switch (MemScope) {
  case ScopeCrossDevice:
  case ScopeDevice:
  case ScopeQueueFamilyKHR:
    Scope = SyncScope::System;
    break;
  case ScopeInvocation:
    Scope = SyncScope::SingleThread;
    break;
  case ScopeWorkgroup:
    Scope = Context->getOrInsertSyncScopeID("workgroup");
    break;
  case ScopeSubgroup:
    Scope = Context->getOrInsertSyncScopeID("wavefront");
    break;
  default:
    llvm_unreachable("Invalid scope");
  }

  return new FenceInst(*Context, Ordering, Scope, BB);
}

Instruction *SPIRVToLLVM::transBarrierFence(SPIRVInstruction *MB,
                                               BasicBlock *BB) {
  assert(BB && "Invalid BB");
  std::string FuncName;
  auto GetIntVal = [](SPIRVValue *Value) {
    return static_cast<SPIRVConstant *>(Value)->getZExtIntValue();
  };

  Instruction *Barrier = nullptr;

  if (MB->getOpCode() == OpMemoryBarrier) {
    auto MemB = static_cast<SPIRVMemoryBarrier *>(MB);

    SPIRVWord MemScope = GetIntVal(MemB->getOpValue(0));
    SPIRVWord MemSema = GetIntVal(MemB->getOpValue(1));

    Barrier = transMemFence(BB, MemSema, MemScope);
  } else if (MB->getOpCode() == OpControlBarrier) {
    auto CtlB = static_cast<SPIRVControlBarrier *>(MB);

    SPIRVWord ExecScope = GetIntVal(CtlB->getExecScope());
    SPIRVWord MemSema = GetIntVal(CtlB->getMemSemantic());
    SPIRVWord MemScope = GetIntVal(CtlB->getMemScope());

    Barrier = transBarrier(BB, ExecScope, MemSema, MemScope);
  } else {
    llvm_unreachable("Invalid instruction");
  }

  if (Barrier) {
    setName(Barrier, MB);

    if (CallInst *Call = dyn_cast<CallInst>(Barrier))
      setAttrByCalledFunc(Call);
  }

  return Barrier;
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

} // namespace SPIRV

bool llvm::readSpirv(Builder *Builder, const ShaderModuleUsage *shaderInfo, std::istream &IS,
                     spv::ExecutionModel EntryExecModel, const char *EntryName,
                     const SPIRVSpecConstMap &SpecConstMap, Module *M,
                     std::string &ErrMsg) {
  assert((EntryExecModel != ExecutionModelKernel) && "Not support ExecutionModelKernel");

  std::unique_ptr<SPIRVModule> BM(SPIRVModule::createSPIRVModule());

  IS >> *BM;

  SPIRVToLLVM BTL(M, BM.get(), SpecConstMap, Builder, shaderInfo);
  bool Succeed = true;
  if (!BTL.translate(EntryExecModel, EntryName)) {
    BM->getError(ErrMsg);
    Succeed = false;
  }

  if (DbgSaveTmpLLVM)
    dumpLLVM(M, DbgTmpLLVMFileName);

  return Succeed;
}

