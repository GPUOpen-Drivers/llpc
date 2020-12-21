//===- SPIRVReader.h - Converts SPIR-V to LLVM ----------------*- C++ -*-===//
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
#ifndef SPIRVREADER_H
#define SPIRVREADER_H

#include "SPIRVInstruction.h"
#include "SPIRVInternal.h"
#include "SPIRVModule.h"
#include "SPIRVToLLVMDbgTran.h"
#include "vkgcDefs.h"
#include "lgc/Builder.h"

namespace llvm {
class Module;
class Type;
class Instruction;
class CallInst;
class BasicBlock;
class Function;
class GlobalVariable;
class LLVMContext;
class LoadInst;
class BranchInst;
class BinaryOperator;
class Value;
} // namespace llvm
using namespace llvm;

namespace SPIRV {
class SPIRVLoopMerge;
class SPIRVToLLVMDbgTran;

class SPIRVToLLVM {
public:
  SPIRVToLLVM(Module *llvmModule, SPIRVModule *theSpirvModule, const SPIRVSpecConstMap &theSpecConstMap,
              llvm::ArrayRef<ConvertingSampler> convertingSamplers, lgc::Builder *builder,
              const Vkgc::ShaderModuleUsage *moduleUsage, const Vkgc::PipelineShaderOptions *shaderOptions);

  DebugLoc getDebugLoc(SPIRVInstruction *bi, Function *f);

  void updateDebugLoc(SPIRVValue *bv, Function *f);

  Type *transType(SPIRVType *bt, unsigned matrixStride = 0, bool columnMajor = true, bool parentIsPointer = false,
                  bool explicitlyLaidOut = false);
  template <spv::Op>
  Type *transTypeWithOpcode(SPIRVType *bt, unsigned matrixStride, bool columnMajor, bool parentIsPointer,
                            bool explicitlyLaidOut);
  std::vector<Type *> transTypeVector(const std::vector<SPIRVType *> &);
  bool translate(ExecutionModel entryExecModel, const char *entryName);
  bool transAddressingModel();

  Value *transValue(SPIRVValue *, Function *f, BasicBlock *, bool createPlaceHolder = true);
  Value *transValueWithoutDecoration(SPIRVValue *, Function *f, BasicBlock *, bool createPlaceHolder = true);
  Value *transAtomicRMW(SPIRVValue *, const AtomicRMWInst::BinOp);
  Constant *transInitializer(SPIRVValue *, Type *);
  template <spv::Op> Value *transValueWithOpcode(SPIRVValue *);
  Value *transLoadImage(SPIRVValue *spvImageLoadPtr);
  Value *loadImageSampler(Type *elementTy, Value *base);
  Value *transImagePointer(SPIRVValue *spvImagePtr);
  Value *getDescPointerAndStride(lgc::ResourceNodeType resType, unsigned descriptorSet, unsigned binding);
  Value *transOpAccessChainForImage(SPIRVAccessChainBase *spvAccessChain);
  Value *indexDescPtr(Type *elementTy, Value *base, Value *index, bool isNonUniform);
  Value *transGroupArithOp(lgc::Builder::GroupArithOp, SPIRVValue *);

  bool transDecoration(SPIRVValue *, Value *);
  bool transShaderDecoration(SPIRVValue *, Value *);
  bool checkContains64BitType(SPIRVType *bt);
  Constant *buildShaderInOutMetadata(SPIRVType *bt, ShaderInOutDecorate &inOutDec, Type *&metaTy);
  Constant *buildShaderBlockMetadata(SPIRVType *bt, ShaderBlockDecorate &blockDec, Type *&mdTy);
  unsigned calcShaderBlockSize(SPIRVType *bt, unsigned blockSize, unsigned matrixStride, bool isRowMajor);
  Value *transGLSLExtInst(SPIRVExtInst *extInst, BasicBlock *bb);
  Value *flushDenorm(Value *val);
  Value *transTrinaryMinMaxExtInst(SPIRVExtInst *extInst, BasicBlock *bb);
  Value *transGLSLBuiltinFromExtInst(SPIRVExtInst *bc, BasicBlock *bb);
  std::vector<Value *> transValue(const std::vector<SPIRVValue *> &, Function *f, BasicBlock *);
  Function *transFunction(SPIRVFunction *f);
  bool transMetadata();
  bool transNonTemporalMetadata(Instruction *i);
  Value *transConvertInst(SPIRVValue *bv, Function *f, BasicBlock *bb);
  Instruction *transBuiltinFromInst(const std::string &funcName, SPIRVInstruction *bi, BasicBlock *bb);
  Instruction *transSPIRVBuiltinFromInst(SPIRVInstruction *bi, BasicBlock *bb);
  Instruction *transBarrierFence(SPIRVInstruction *bi, BasicBlock *bb);

  // Struct used to pass information in and out of getImageDesc.
  struct ExtractedImageInfo {
    BasicBlock *bb;
    const SPIRVTypeImageDescriptor *desc;
    unsigned dim;          // lgc::Builder dimension
    unsigned flags;        // lgc::Builder image call flags
    Value *imageDesc;      // Image descriptor (first plane if multi-plane)
    Value *imageDescArray; // Array of image descriptors for multi-plane
    Value *fmaskDesc;
    Value *samplerDesc;
  };

  // Load image and/or sampler descriptors, and get information from the image
  // type.
  void getImageDesc(SPIRVValue *bImageInst, ExtractedImageInfo *info);

  // Set up address operand array for image sample/gather builder call.
  void setupImageAddressOperands(SPIRVInstruction *bi, unsigned maskIdx, bool hasProj, MutableArrayRef<Value *> addr,
                                 ExtractedImageInfo *imageInfo, Value **sampleNum);

  // Handle fetch/read/write/atomic aspects of coordinate.
  void handleImageFetchReadWriteCoord(SPIRVInstruction *bi, ExtractedImageInfo *imageInfo,
                                      MutableArrayRef<Value *> addr, bool enableMultiView = true);

  // Translate SPIR-V image atomic operations to LLVM function calls
  Value *transSPIRVImageAtomicOpFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translates SPIR-V fragment mask operations to LLVM function calls
  Value *transSPIRVFragmentMaskFetchFromInst(SPIRVInstruction *bi, BasicBlock *bb);
  Value *transSPIRVFragmentFetchFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translate image sample to LLVM IR
  Value *transSPIRVImageSampleFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translate image gather to LLVM IR
  Value *transSPIRVImageGatherFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translate image fetch/read to LLVM IR
  Value *transSPIRVImageFetchReadFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translate image write to LLVM IR
  Value *transSPIRVImageWriteFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translate OpImageQueryLevels to LLVM IR
  Value *transSPIRVImageQueryLevelsFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translate OpImageQuerySamples to LLVM IR
  Value *transSPIRVImageQuerySamplesFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translate OpImageQuerySize/OpImageQuerySizeLod to LLVM IR
  Value *transSPIRVImageQuerySizeFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  // Translate OpImageQueryLod to LLVM IR
  Value *transSPIRVImageQueryLodFromInst(SPIRVInstruction *bi, BasicBlock *bb);

  Value *createLaunderRowMajorMatrix(Value *const);
  Value *addLoadInstRecursively(SPIRVType *const, Value *const, bool, bool, bool);
  void addStoreInstRecursively(SPIRVType *const, Value *const, Value *const, bool, bool, bool);
  Constant *buildConstStoreRecursively(SPIRVType *const, Type *const, Constant *const);

  // Post-process translated LLVM module to undo row major matrices.
  bool postProcessRowMajorMatrix();

  typedef DenseMap<SPIRVType *, Type *> SPIRVToLLVMTypeMap;
  typedef DenseMap<SPIRVValue *, Value *> SPIRVToLLVMValueMap;
  typedef DenseMap<SPIRVValue *, Value *> SPIRVBlockToLLVMStructMap;
  typedef DenseMap<SPIRVFunction *, Function *> SPIRVToLLVMFunctionMap;
  typedef DenseMap<GlobalVariable *, SPIRVBuiltinVariableKind> BuiltinVarMap;
  typedef DenseMap<SPIRVType *, SmallVector<unsigned, 8>> RemappedTypeElementsMap;

  // A SPIRV value may be translated to a load instruction of a placeholder
  // global variable. This map records load instruction of these placeholders
  // which are supposed to be replaced by the real values later.
  typedef std::map<SPIRVValue *, LoadInst *> SPIRVToLLVMPlaceholderMap;

private:
  Module *m_m;
  BuiltinVarMap m_builtinGvMap;
  LLVMContext *m_context;
  lgc::Builder *m_builder;
  SPIRVModule *m_bm;
  bool m_enableXfb;
  bool m_enableGatherLodNz;
  ShaderFloatControlFlags m_fpControlFlags;
  SPIRVFunction *m_entryTarget;
  const SPIRVSpecConstMap &m_specConstMap;
  llvm::ArrayRef<ConvertingSampler> m_convertingSamplers;
  SPIRVToLLVMTypeMap m_typeMap;
  SPIRVToLLVMValueMap m_valueMap;
  SPIRVToLLVMFunctionMap m_funcMap;
  SPIRVBlockToLLVMStructMap m_blockMap;
  SPIRVToLLVMPlaceholderMap m_placeholderMap;
  SPIRVToLLVMDbgTran m_dbgTran;
  std::map<std::string, unsigned> m_mangleNameToIndex;
  RemappedTypeElementsMap m_remappedTypeElements;
  DenseMap<Type *, bool> m_typesWithPadMap;
  DenseMap<Type *, uint64_t> m_typeToStoreSize;
  DenseMap<std::pair<SPIRVType *, unsigned>, Type *> m_overlappingStructTypeWorkaroundMap;
  DenseMap<std::pair<BasicBlock *, BasicBlock *>, unsigned> m_blockPredecessorToCount;
  const Vkgc::ShaderModuleUsage *m_moduleUsage;
  const Vkgc::PipelineShaderOptions *m_shaderOptions;
  unsigned m_spirvOpMetaKindId;
  unsigned m_execModule;
  lgc::Builder *getBuilder() const { return m_builder; }

  Type *mapType(SPIRVType *bt, Type *t) {
    m_typeMap[bt] = t;
    return t;
  }

  void recordRemappedTypeElements(SPIRVType *bt, unsigned from, unsigned to);

  bool isRemappedTypeElements(SPIRVType *bt) const { return m_remappedTypeElements.count(bt) > 0; }

  unsigned lookupRemappedTypeElements(SPIRVType *bt, unsigned from) {
    assert(m_remappedTypeElements.count(bt) > 0);
    assert(m_remappedTypeElements[bt].size() > from);
    return m_remappedTypeElements[bt][from];
  }

  Type *getPadType(unsigned bytes) { return ArrayType::get(getBuilder()->getInt8Ty(), bytes); }

  Type *recordTypeWithPad(Type *const t, bool isMatrixRow = false) {
    m_typesWithPadMap[t] = isMatrixRow;
    return t;
  }

  bool isTypeWithPad(Type *const t) const { return m_typesWithPadMap.count(t) > 0; }

  bool isTypeWithPadRowMajorMatrix(Type *const t) const { return m_typesWithPadMap.lookup(t); }

  // Returns a cached type store size. If there is no entry for the given type,
  // its store size is calculated and added to the cache.
  uint64_t getTypeStoreSize(Type *const t);

  // If a value is mapped twice, the existing mapped value is a placeholder,
  // which must be a load instruction of a global variable whose name starts
  // with kPlaceholderPrefix.
  Value *mapValue(SPIRVValue *bv, Value *v);

  // Used to keep track of the number of incoming edges to a block from each
  // of the predecessor.
  void recordBlockPredecessor(BasicBlock *block, BasicBlock *predecessorBlock) {
    assert(block);
    assert(predecessorBlock);
    m_blockPredecessorToCount[{block, predecessorBlock}] += 1;
  }

  unsigned getBlockPredecessorCounts(BasicBlock *block, BasicBlock *predecessor);

  bool isSPIRVBuiltinVariable(GlobalVariable *gv, SPIRVBuiltinVariableKind *kind = nullptr);

  // Change this if it is no longer true.
  bool isFuncNoUnwind() const { return true; }
  bool isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction *bi) const;

  Value *mapFunction(SPIRVFunction *bf, Function *f) {
    m_funcMap[bf] = f;
    return f;
  }

  Value *getTranslatedValue(SPIRVValue *bv);

  SPIRVErrorLog &getErrorLog() { return m_bm->getErrorLog(); }

  void setCallingConv(CallInst *call) {
    Function *f = call->getCalledFunction();
    assert(f);
    call->setCallingConv(f->getCallingConv());
  }

  void setAttrByCalledFunc(CallInst *call);
  Type *transFPType(SPIRVType *t);
  FastMathFlags getFastMathFlags(SPIRVValue *bv);
  void setFastMathFlags(SPIRVValue *bv);
  void setFastMathFlags(Value *val);
  BinaryOperator *transShiftLogicalBitwiseInst(SPIRVValue *bv, BasicBlock *bb, Function *f);
  Instruction *transCmpInst(SPIRVValue *bv, BasicBlock *bb, Function *f);

  void setName(llvm::Value *v, SPIRVValue *bv);
  void setLLVMLoopMetadata(SPIRVLoopMerge *lm, BranchInst *bi);
  template <class Source, class Func> bool foreachFuncCtlMask(Source, Func);
  llvm::GlobalValue::LinkageTypes transLinkageType(const SPIRVValue *v);

  Instruction *transBarrier(BasicBlock *bb, SPIRVWord execScope, SPIRVWord memSema, SPIRVWord memScope);

  Instruction *transMemFence(BasicBlock *bb, SPIRVWord memSema, SPIRVWord memScope);
  void truncConstantIndex(std::vector<Value *> &indices, BasicBlock *bb);

  Value *ConvertingSamplerSelectLadderHelper(Value *result, Value *convertingSamplerIdx,
                                             std::function<Value *(Value *)> createImageOp);
}; // class SPIRVToLLVM

} // namespace SPIRV

#endif
