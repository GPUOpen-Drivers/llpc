/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  LowerCooperativeMatrix.cpp
 * @brief LGC source file : Lower CooperativeMatrix manager, and pass that uses it
 ***********************************************************************************************************************
 */
#include "lgc/patch/LowerCooperativeMatrix.h"
#include "lgc/Builder.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-lower-cooperative-matrix"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Run the patch cooperative matrix pass on a module
//
// @param [in/out] module :  LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The Analyses that are still valid after this pass)
PreservedAnalyses LowerCooperativeMatrix::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);

  LLVM_DEBUG(dbgs() << "Run the pass Patch-Cooperative-Matrix\n");
  Patch::init(&module);
  m_pipelineState = pipelineState;
  m_pipelineShaders = &pipelineShaders;
  m_shaderStage = ShaderStage::Compute;
  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  processCoopRowAccFunction(module);
  processCoopMatrixFunction(module);

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

// =====================================================================================================================
// Visit the cooperative matrix ops on module
//
// @param [in] module :  LLVM module to be run on
void LowerCooperativeMatrix::processCoopMatrixFunction(Module &module) {
  static auto visitor = llvm_dialects::VisitorBuilder<LowerCooperativeMatrix>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixLengthOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixExtractOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixInsertOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixFillOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixLoadOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixStoreOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixConvertOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixTransposeOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixBinaryOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixTimesScalarOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixMulAddOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixPackOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeMatrixUnPackOp)
                            .build();

  visitor.visit(*this, module);

  for (auto callInst : m_coopMatrixCalls) {
    callInst->dropAllReferences();
    callInst->eraseFromParent();
  }
  m_coopMatrixCalls.clear();
}

// =====================================================================================================================
// Determine properties of the cooperative matrix type depending on element type, layout, and wave size.
//
// @param elemType : the matrix element type
// @param layout : the matrix layout
// @returns : the type properties
LowerCooperativeMatrix::TypeProperties LowerCooperativeMatrix::getTypeProperties(CooperativeMatrixElementType elemType,
                                                                                 CooperativeMatrixLayout layout) const {
  TypeProperties props;

  props.matrixElementStride = 1;

  switch (elemType) {
  case CooperativeMatrixElementType::Float32:
  case CooperativeMatrixElementType::Int32:
    props.numMatrixElements = 8;
    props.numMatrixWords = 8;
    break;
  case CooperativeMatrixElementType::Float16:
  case CooperativeMatrixElementType::Float16Packed:
  case CooperativeMatrixElementType::Int16:
    props.numMatrixElements = 16;
    props.numMatrixWords = 8;
    break;
  case CooperativeMatrixElementType::Int8:
    props.numMatrixElements = 16;
    props.numMatrixWords = 4;
    break;
  default:
    llvm_unreachable("unknown element type");
  }

  auto waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  if (layout == CooperativeMatrixLayout::FactorMatrixLayout) {
    assert(elemType != CooperativeMatrixElementType::Float32 && elemType != CooperativeMatrixElementType::Int32);
    props.numFlatElements = 16;
  } else if (layout == CooperativeMatrixLayout::AccumulatorMatrixLayout) {
    if (elemType == CooperativeMatrixElementType::Float16 || elemType == CooperativeMatrixElementType::Int16) {
      props.matrixElementStride = 2;
    }
    if (elemType == CooperativeMatrixElementType::Float16Packed) {
      props.numFlatElements = waveSize == 32 ? 16 : 8;
    } else {
      props.numFlatElements = waveSize == 32 ? 8 : 4;
    }
  } else if (layout == CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout ||
             layout == CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) {
    props.numFlatElements = 8;
  } else {
    llvm_unreachable("Unsupported layout!");
  }

  return props;
}

// =====================================================================================================================
// Create cooperative Matrix data(C/D:V8/V4 A/B: V8/V4) from vector value(C/D wave32:V8 wave64:V4  A/B: V16)
//
// @param builder : the builder to use
// @param vecValue : Vector Value which maybe V16.
// @param elemType : Element type for the matrix.
// @param layout : Identify whether this matrix is A/B or C/D
Value *LowerCooperativeMatrix::convFlatVecToCoopMatrixVec(BuilderCommon &builder, Value *vecValue,
                                                          CooperativeMatrixElementType elemType,
                                                          CooperativeMatrixLayout layout) {
  auto props = getTypeProperties(elemType, layout);

  if (props.numMatrixElements > props.numFlatElements) {
    SmallVector<int, 16> mask;
    for (unsigned i = 0; i < props.numMatrixElements / props.matrixElementStride; ++i) {
      mask.push_back(i);
      for (unsigned j = 1; j < props.matrixElementStride; ++j)
        mask.push_back(-1);
    }
    vecValue = builder.CreateShuffleVector(vecValue, PoisonValue::get(vecValue->getType()), mask);
  }

  Type *wordTy = vecValue->getType()->isIntOrIntVectorTy() ? builder.getInt32Ty() : builder.getFloatTy();
  return builder.CreateBitCast(vecValue, FixedVectorType::get(wordTy, props.numMatrixWords));
}

// =====================================================================================================================
// Create vector value(C/D wave32:V8 wave64:V4  A/B: V16) from cooperative Matrix data(C/D:V8/V4 A/B: V8/V4)
//
// @param builder : the builder to use
// @param matrixValue : Vector Value which maybe V16.
// @param elemType : Element type for the matrix.
// @param layout : Identify whether this matrix is A/B or C/D
Value *LowerCooperativeMatrix::convCoopMatrixVecToFlatVec(BuilderCommon &builder, Value *matrixValue,
                                                          CooperativeMatrixElementType elemType,
                                                          CooperativeMatrixLayout layout) {
  auto props = getTypeProperties(elemType, layout);

  Type *flatType = FixedVectorType::get(builder.transCooperativeMatrixElementType(elemType), props.numMatrixElements);
  Value *tmp = builder.CreateBitCast(matrixValue, flatType);

  if (props.numFlatElements < props.numMatrixElements) {
    SmallVector<int, 8> mask;
    for (unsigned i = 0; i < props.numFlatElements; ++i)
      mask.push_back(i * props.matrixElementStride);
    tmp = builder.CreateShuffleVector(tmp, PoisonValue::get(tmp->getType()), mask);
  }

  return tmp;
}

// =====================================================================================================================
// Load contiguous elements from the specified location of the memory.
// @param layout : This is identify for factor(A/B) or accumulator(C) for 16 bit element matrix.
// @param elemType : The element type for the matrix.
// @param waveSize : Identify it's in wave32 or wave64.
// @param stride : The stride in bytes in memory between the first elements of consecutive rows (orcolumns) in the
// source data. Guaranteed to be a multiple of the matrix element size.
// @param isColMajor : Identify the order for the data stored in memory, col-major/row-major
// @param insertPos : Where to insert the instruction
LowerCooperativeMatrix::ComputeAddressInfo
LowerCooperativeMatrix::computeAddressing(CooperativeMatrixLayout layout, CooperativeMatrixElementType elemType,
                                          int waveSize, Value *stride, bool isColMajor, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *threadId = getLaneNumber(builder);
  ComputeAddressInfo addrInfo;
  Value *rowOffsetInFirstVgpr = nullptr;
  Value *colOffsetPerLane = builder.CreateSRem(threadId, builder.getInt32(16));
  addrInfo.microStep = builder.getInt32(0);
  addrInfo.microCount = 1;
  (void)elemType;

  if (layout == CooperativeMatrixLayout::FactorMatrixLayout) {
    rowOffsetInFirstVgpr = builder.getInt32(0);
    addrInfo.macroStep = builder.getInt32(1);
  } else if (layout == CooperativeMatrixLayout::AccumulatorMatrixLayout) {
    rowOffsetInFirstVgpr = builder.CreateUDiv(threadId, builder.getInt32(16));
    addrInfo.macroStep = (waveSize == 64 ? builder.getInt32(4) : builder.getInt32(2));
  } else if (layout == CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout) {
    rowOffsetInFirstVgpr = builder.CreateUDiv(builder.CreateSRem(threadId, builder.getInt32(32)), builder.getInt32(16));
    addrInfo.macroStep = builder.getInt32(2);
  } else if (layout == CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) {
    // For 16bit@Accumulator@gfx10：lane_0: {0_0,1_0,4_0,5_0,8_0,9_0,12_0,13_0}
    // lane_16: {2_0,3_0,6_0,7_0,10_0,11_0,14_0,15_0} on lane_16.
    Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
    Value *evenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));
    addrInfo.microCount = 2;
    rowOffsetInFirstVgpr = builder.CreateSelect(evenGroup, builder.getInt32(0), builder.getInt32(2));
    addrInfo.macroStep = builder.getInt32(4);
    addrInfo.microStep = builder.getInt32(1);
  } else {
    llvm_unreachable("This layout is not supported now.");
  }

  if (isColMajor) {
    addrInfo.base = builder.CreateAdd(rowOffsetInFirstVgpr, builder.CreateMul(colOffsetPerLane, stride));
  } else {
    addrInfo.base = builder.CreateAdd(builder.CreateMul(rowOffsetInFirstVgpr, stride), colOffsetPerLane);
    addrInfo.macroStep = builder.CreateMul(addrInfo.macroStep, stride);
    addrInfo.microStep = builder.CreateMul(addrInfo.microStep, stride);
  }

  return addrInfo;
}

// =====================================================================================================================
// Visit "CooperativeMatrixLengthOp" instruction
//
// @param matrixlength : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixLengthOp(CooperativeMatrixLengthOp &matrixlength) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&matrixlength);
  auto waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  auto layout = matrixlength.getLayout();
  unsigned length = 0;
  switch (layout) {
  case CooperativeMatrixLayout::FactorMatrixLayout:
    length = 16;
    break;
  case CooperativeMatrixLayout::AccumulatorMatrixLayout: {
    length = (waveSize == 32) ? 8 : 4;
    break;
  }
  case CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout:
  case CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout:
    length = 8;
    break;
  default:
    llvm_unreachable("unhandled matrix layout");
  }
  m_coopMatrixCalls.push_back(&matrixlength);
  matrixlength.replaceAllUsesWith(builder.getInt32(length));
}

// =====================================================================================================================
// Visit "CooperativeMatrixLoadOp" instruction
//
// @param load : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixLoadOp(CooperativeMatrixLoadOp &load) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&load);

  auto shaderStage = getShaderStage(builder.GetInsertBlock()->getParent());
  auto waveSize = m_pipelineState->getShaderWaveSize(shaderStage.value());
  assert(waveSize == 32 || waveSize == 64);

  auto elemType = load.getElemType();
  Value *dataPtr = load.getPointer();
  Value *stride = load.getStride();
  auto memoryAccess = load.getMemoryAccess();
  auto layout = load.getLayout();
  auto isColMajor = load.getColMajor();
  auto alignment = load.getAlignment();

  // Calc element offset in memory
  Type *elemTy = builder.transCooperativeMatrixElementType(elemType);
  const unsigned dataBitwidth = elemTy->getScalarSizeInBits();
  const unsigned addrSpace = dataPtr->getType()->getPointerAddressSpace();
  assert(addrSpace == ADDR_SPACE_LOCAL || addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER || addrSpace == ADDR_SPACE_GLOBAL);

  stride = builder.CreateExactSDiv(stride, builder.getInt32(dataBitwidth / 8));

  // calc memoryAccess
  bool isVolatile = memoryAccess & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessVolatileMask);
  bool isCoherent = memoryAccess & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessCoherentMask);
  bool isTemporal = memoryAccess & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessTemporalMask);

  auto props = getTypeProperties(elemType, layout);

  auto addrInfo = computeAddressing(layout, elemType, waveSize, stride, isColMajor, &load);
  Value *vecVal = PoisonValue::get(FixedVectorType::get(elemTy, props.numFlatElements));
  for (unsigned idx = 0; idx < props.numFlatElements; ++idx) {
    Value *macroOffset = builder.CreateMul(addrInfo.macroStep, builder.getInt32(idx / addrInfo.microCount));
    Value *microOffset = builder.CreateMul(addrInfo.microStep, builder.getInt32(idx % addrInfo.microCount));
    Value *offsetInRowCol = builder.CreateAdd(macroOffset, microOffset);
    Value *offsetInMatrix = builder.CreateAdd(addrInfo.base, offsetInRowCol);
    Value *elePtr = builder.CreateGEP(elemTy, dataPtr, offsetInMatrix);
    Value *eleVal = nullptr;
    if (isColMajor) {
      // For colMajor@B/C and rowMajor@A, as the elements of one lane are continuous, add the alignments for
      // merging load/store instructions on backend later.
      unsigned constantOffsetInRowCol = cast<ConstantInt>(offsetInRowCol)->getZExtValue();
      Align compAlignment = commonAlignment(Align(alignment), constantOffsetInRowCol);
      eleVal = builder.CreateAlignedLoad(elemTy, elePtr, compAlignment, isVolatile);
    } else {
      // For rowMajor@B/C and colMajor@A, as the elements of one lane aren't continuous, no alignments needed.
      eleVal = builder.CreateLoad(elemTy, elePtr, isVolatile);
    }
    if (isCoherent && !(addrSpace == ADDR_SPACE_LOCAL && dataBitwidth < 32))
      cast<LoadInst>(eleVal)->setAtomic(AtomicOrdering::Unordered);
    if (isTemporal)
      cast<LoadInst>(eleVal)->setMetadata(LLVMContext::MD_nontemporal, MDNode::get(builder.getContext(), {}));
    vecVal = builder.CreateInsertElement(vecVal, eleVal, idx);
  }

  Value *coMatrix = convFlatVecToCoopMatrixVec(builder, vecVal, elemType, layout);
  m_coopMatrixCalls.push_back(&load);
  load.replaceAllUsesWith(coMatrix);
}

// =====================================================================================================================
// Visit "CooperativeMatrixStoreOp" instruction
//
// @param store : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixStoreOp(CooperativeMatrixStoreOp &store) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&store);

  auto elemType = store.getElemType();
  Value *dataPtr = store.getPointer();
  Value *stride = store.getStride();
  auto memoryAccess = store.getMemoryAccess();
  auto layout = store.getLayout();
  auto isColMajor = store.getColMajor();
  auto alignment = store.getAlignment();
  Value *vecVal = store.getStoreValue();
  auto shaderStage = getShaderStage(builder.GetInsertBlock()->getParent());
  auto waveSize = m_pipelineState->getShaderWaveSize(shaderStage.value());
  assert(waveSize == 32 || waveSize == 64);

  // Calc element offset in memory
  Type *elemTy = builder.transCooperativeMatrixElementType(elemType);
  const unsigned dataBitwidth = elemTy->getScalarSizeInBits();
  const unsigned addrSpace = dataPtr->getType()->getPointerAddressSpace();
  assert(addrSpace == ADDR_SPACE_LOCAL || addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER || addrSpace == ADDR_SPACE_GLOBAL);

  stride = builder.CreateExactSDiv(stride, builder.getInt32(dataBitwidth / 8));

  // calc memoryAccess
  bool isVolatile = memoryAccess & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessVolatileMask);
  bool isCoherent = memoryAccess & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessCoherentMask);
  bool isTemporal = memoryAccess & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessTemporalMask);

  auto props = getTypeProperties(elemType, layout);
  auto addrInfo = computeAddressing(layout, elemType, waveSize, stride, isColMajor, &store);

  vecVal = convCoopMatrixVecToFlatVec(builder, vecVal, elemType, layout);

  for (unsigned idx = 0; idx < props.numFlatElements; ++idx) {
    Value *macroOffset = builder.CreateMul(addrInfo.macroStep, builder.getInt32(idx / addrInfo.microCount));
    Value *microOffset = builder.CreateMul(addrInfo.microStep, builder.getInt32(idx % addrInfo.microCount));
    Value *offsetInRowCol = builder.CreateAdd(macroOffset, microOffset);
    Value *offsetInMatrix = builder.CreateAdd(addrInfo.base, offsetInRowCol);
    Value *elePtr = builder.CreateGEP(elemTy, dataPtr, offsetInMatrix);
    Value *oneElement = builder.CreateExtractElement(vecVal, idx);
    StoreInst *st = nullptr;
    if (isColMajor) {
      // Add alignment same with Load
      unsigned constantOffsetInRowCol = cast<ConstantInt>(offsetInRowCol)->getZExtValue();
      Align compAlignment = commonAlignment(Align(alignment), constantOffsetInRowCol);
      st = builder.CreateAlignedStore(oneElement, elePtr, compAlignment, isVolatile);
    } else {
      st = builder.CreateStore(oneElement, elePtr, isVolatile);
    }
    if (isCoherent && !(addrSpace == ADDR_SPACE_LOCAL && dataBitwidth < 32))
      st->setAtomic(AtomicOrdering::Unordered);
    if (isTemporal)
      st->setMetadata(LLVMContext::MD_nontemporal, MDNode::get(builder.getContext(), {}));
  }

  m_coopMatrixCalls.push_back(&store);
}

// =====================================================================================================================
// Visit "CooperativeMatrixFillOp" instruction
//
// @param fill : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixFillOp(CooperativeMatrixFillOp &fill) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&fill);

  auto elemType = fill.getElemType();
  auto layout = fill.getLayout();
  Value *value = fill.getScalar();
  auto props = getTypeProperties(elemType, layout);
  Type *flatType = FixedVectorType::get(builder.transCooperativeMatrixElementType(elemType), props.numMatrixElements);

  Value *vec = PoisonValue::get(flatType);
  for (unsigned idx = 0; idx < props.numMatrixElements; idx++)
    vec = builder.CreateInsertElement(vec, value, idx);

  Value *fillValue = convFlatVecToCoopMatrixVec(builder, vec, elemType, layout);

  m_coopMatrixCalls.push_back(&fill);
  fill.replaceAllUsesWith(fillValue);
}

// =====================================================================================================================
// Visit "CooperativeMatrixExtractOp" instruction
//
// @param extract : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixExtractOp(CooperativeMatrixExtractOp &extract) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&extract);

  auto matrix = extract.getMatrix();
  auto elemType = extract.getElemType();
  auto layout = extract.getLayout();
  auto index = extract.getIndex();
  Value *vec = convCoopMatrixVecToFlatVec(builder, matrix, elemType, layout);

  // This is a hacky workaround to the fact that for SPV_NV_cooperative_matrix, we have to support matrix length as
  // a specialization constant even though, at the time of specialization constant lowering, we don't yet know the
  // wave size. We should remove this once a healther KHR extension has been released.
  if (layout == CooperativeMatrixLayout::AccumulatorMatrixLayout &&
      m_pipelineState->getShaderWaveSize(m_shaderStage) == 64) {
    unsigned length = cast<FixedVectorType>(vec->getType())->getNumElements();
    index = builder.CreateAnd(index, builder.getInt32(length - 1));
  }

  Value *elementValue = builder.CreateExtractElement(vec, index);
  m_coopMatrixCalls.push_back(&extract);
  extract.replaceAllUsesWith(elementValue);
}

// =====================================================================================================================
// Visit "CooperativeMatrixInsertOp" instruction
//
// @param insert : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixInsertOp(CooperativeMatrixInsertOp &insert) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&insert);

  auto matrix = insert.getMatrix();
  auto elemType = insert.getElemType();
  auto layout = insert.getLayout();
  auto index = insert.getIndex();
  auto value = insert.getInsertValue();
  Value *vec = convCoopMatrixVecToFlatVec(builder, matrix, elemType, layout);

  // This is a hacky workaround to the fact that for SPV_NV_cooperative_matrix, we have to support matrix length as
  // a specialization constant even though, at the time of specialization constant lowering, we don't yet know the
  // wave size. We should remove this once a healther KHR extension has been released.
  if (layout == CooperativeMatrixLayout::AccumulatorMatrixLayout &&
      m_pipelineState->getShaderWaveSize(m_shaderStage) == 64) {
    unsigned length = cast<FixedVectorType>(vec->getType())->getNumElements();
    Value *outOfBounds = builder.CreateICmpUGE(index, builder.getInt32(length));
    index = builder.CreateAnd(index, builder.getInt32(length - 1));
    Value *newVec = builder.CreateInsertElement(vec, value, index);
    vec = builder.CreateSelect(outOfBounds, vec, newVec);
  } else {
    vec = builder.CreateInsertElement(vec, value, index);
  }

  Value *out = convFlatVecToCoopMatrixVec(builder, vec, elemType, layout);
  m_coopMatrixCalls.push_back(&insert);
  insert.replaceAllUsesWith(out);
}

// =====================================================================================================================
// Create cooperative matrix conversion without any reshape operations
// Element-wise-conversion
// @param castOp : The cast Opcode.
// @param source : The source cooperative matrix.
// @param dstElemType : Source matrix's element type.
// @param dstElemType : Destination matrix's element type.
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixConvertInternal(CastInst::CastOps castOp, Value *source,
                                                                CooperativeMatrixElementType srcElemType,
                                                                CooperativeMatrixElementType dstElemType,
                                                                const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = nullptr;
  const unsigned vecSize = cast<FixedVectorType>(source->getType())->getNumElements();
  Type *dstType = FixedVectorType::get(builder.transCooperativeMatrixElementType(dstElemType), vecSize);

  if ((srcElemType == CooperativeMatrixElementType::Float16 || srcElemType == CooperativeMatrixElementType::Float32) &&
      (castOp == Instruction::FPToUI || castOp == Instruction::FPToSI)) {
    // FIXME: fp16's range is covered by i32. So `fptoi half` can convert
    // to i32 first following a sext/zext to target integer type.
    // Fix the error in: dEQP-VK.compute.cooperative_matrix.nv.convert.input_float16/32_t_output_uint8_t*
    resultValue =
        builder.CreateCast(castOp, source, FixedVectorType::get(builder.getInt32Ty(), vecSize), "ConvertIntoInt32");
    if (builder.transCooperativeMatrixElementType(dstElemType)->getScalarSizeInBits() < 32) {
      resultValue = builder.CreateTrunc(resultValue, dstType);
    }
  } else {
    resultValue = builder.CreateCast(castOp, source, dstType, "castOpConvert");
  }

  return resultValue;
}

// =====================================================================================================================
// Visit "CooperativeMatrixConvertOp" instruction
//
// @param convert : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixConvertOp(CooperativeMatrixConvertOp &convert) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&convert);
  Value *resultValue = nullptr;
  Value *threadId = getLaneNumber(builder);
  CastInst::CastOps castOp = static_cast<CastInst::CastOps>(convert.getCastOp());
  auto srcLayout = convert.getSrcLayout();
  auto dstLayout = convert.getDstLayout();
  auto source = convert.getSource();
  auto srcElemType = convert.getSrcElemType();
  auto dstElemType = convert.getDstElemType();

  if (castOp == 0) { // Only reshape on 16bits, not do convert
    if ((srcLayout == CooperativeMatrixLayout::AccumulatorMatrixLayout) &&
        (dstLayout == CooperativeMatrixLayout::FactorMatrixLayout)) {
      // After mulAdd, the type for the matrix waiting to reshape is 8*float here
      const unsigned vecNums = cast<FixedVectorType>(source->getType())->getNumElements();
      source = builder.CreateBitCast(source, FixedVectorType::get(builder.getInt32Ty(), vecNums));
    }
    resultValue = cooperativeMatrixReshape16BitElementGfx1011(source, srcElemType, srcLayout, dstLayout, threadId,
                                                              convert.getName(), &convert);
  } else {
    unsigned numSrcBit = builder.transCooperativeMatrixElementType(srcElemType)->getScalarSizeInBits();
    unsigned numDstBit = builder.transCooperativeMatrixElementType(dstElemType)->getScalarSizeInBits();

    // Step 1: Some cases need change the layout due to different element types before conversion.
    if ((numSrcBit < numDstBit) && (srcLayout != dstLayout)) {
      // Need Reshape from A/B layout to C/D layout
      // This interface will do cooperativeVecToflatVec internally except 8bit reshape.
      source = cooperativeMatrixReshapeBeforeConvert(source, srcElemType, dstElemType, srcLayout, dstLayout,
                                                     convert.getName(), &convert);
    } else {
      // For 16bit->32bit on Gfx11, no reshape needed as it will always in 	AccumulatorMatrixLayout
      source = convCoopMatrixVecToFlatVec(builder, source, srcElemType, srcLayout);
    }

    // Step 2: Just do flatElement conversion without any layout change.
    resultValue =
        cooperativeMatrixConvertInternal(castOp, source, srcElemType, dstElemType, convert.getName(), &convert);

    // Step 3: Some cases need change the layout due to different element types after conversion.
    if ((numSrcBit > numDstBit) && (srcLayout != dstLayout)) {
      // All these reshape interfaces will return N*packetTy.
      // Need Reshape from A/B layout to C/D layout
      resultValue = cooperativeMatrixReshapeAfterConvert(resultValue, srcElemType, dstElemType, srcLayout, dstLayout,
                                                         convert.getName(), &convert);
    } else {
      resultValue = convFlatVecToCoopMatrixVec(builder, resultValue, dstElemType, dstLayout);
    }
  }
  m_coopMatrixCalls.push_back(&convert);
  convert.replaceAllUsesWith(resultValue);
}

// =====================================================================================================================
// Visit "CooperativeMatrixBinaryOp" instruction
//
// @param binary : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixBinaryOp(CooperativeMatrixBinaryOp &binary) {
  Value *lhs = binary.getLhs();
  Value *rhs = binary.getRhs();
  assert(lhs->getType()->isVectorTy() && lhs->getType() == rhs->getType() || rhs->getType()->isVectorTy());
  CooperativeMatrixArithOp coopMatArithOp = binary.getArithOp();
  auto elemType = binary.getElemType();
  auto layout = binary.getLayout();
  Value *vcResult;
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&binary);

  lhs = convCoopMatrixVecToFlatVec(builder, lhs, elemType, layout);
  rhs = convCoopMatrixVecToFlatVec(builder, rhs, elemType, layout);
  switch (coopMatArithOp) {
  case CooperativeMatrixArithOp::IAdd:
    vcResult = builder.CreateAdd(lhs, rhs);
    break;
  case CooperativeMatrixArithOp::FAdd:
    vcResult = builder.CreateFAdd(lhs, rhs);
    break;
  case CooperativeMatrixArithOp::ISub:
    vcResult = builder.CreateSub(lhs, rhs);
    break;
  case CooperativeMatrixArithOp::FSub:
    vcResult = builder.CreateFSub(lhs, rhs);
    break;
  case CooperativeMatrixArithOp::IMul:
    vcResult = builder.CreateMul(lhs, rhs);
    break;
  case CooperativeMatrixArithOp::FMul:
    vcResult = builder.CreateFMul(lhs, rhs);
    break;
  case CooperativeMatrixArithOp::FDiv:
    vcResult = builder.CreateFDiv(lhs, rhs);
    break;
  case CooperativeMatrixArithOp::SDiv:
    vcResult = builder.CreateSDiv(lhs, rhs);
    break;
  case CooperativeMatrixArithOp::UDiv:
    vcResult = builder.CreateUDiv(lhs, rhs);
    break;
  default:
    llvm_unreachable("unsupported binary operation for cooprative matrix!"); // Rem/Mod is not supported currently.
  }

  Value *coopMatResult = convFlatVecToCoopMatrixVec(builder, vcResult, elemType, layout);
  m_coopMatrixCalls.push_back(&binary);
  binary.replaceAllUsesWith(coopMatResult);
}

// =====================================================================================================================
// Visit "CooperativeMatrixTimesScalarOp" instruction
//
// @param timesScalar : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixTimesScalarOp(CooperativeMatrixTimesScalarOp &timesScalar) {
  Value *matrix = timesScalar.getMatrix();
  assert(matrix->getType()->getScalarType()->isIntegerTy() || matrix->getType()->getScalarType()->isFloatTy());
  auto elemType = timesScalar.getElemType();
  auto layout = timesScalar.getLayout();
  Value *scalar = timesScalar.getScalar();

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&timesScalar);

  Value *vcFlat = convCoopMatrixVecToFlatVec(builder, matrix, elemType, layout);
  const unsigned numElems = cast<FixedVectorType>(vcFlat->getType())->getNumElements();
  const bool packedScalarVec = scalar->getType()->isVectorTy();
  const auto shuffleIndices = numElems == 16 ? SmallVector<int>({0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1})
                                             : SmallVector<int>({0, 1, 0, 1, 0, 1, 0, 1});
  auto splat = packedScalarVec ? builder.CreateShuffleVector(scalar, shuffleIndices)
                               : builder.CreateVectorSplat(numElems, scalar);
  Value *vcFlatResult;
  if ((elemType == CooperativeMatrixElementType::Float16) || (elemType == CooperativeMatrixElementType::Float32) ||
      (elemType == CooperativeMatrixElementType::Float16Packed)) {
    vcFlatResult = builder.CreateFMul(vcFlat, splat);
  } else {
    vcFlatResult = builder.CreateMul(vcFlat, splat);
  }
  Value *coopMatResult = convFlatVecToCoopMatrixVec(builder, vcFlatResult, elemType, layout);
  m_coopMatrixCalls.push_back(&timesScalar);
  timesScalar.replaceAllUsesWith(coopMatResult);
}

// =====================================================================================================================
// Create cooperative matrix reshape operation only for the element is float16
//
// @param source : The first operand and it should be a cooperative matrix.
// @param srcElemType : The component type of the matrix.
// @param srcLayout : Identify whether it's A/B or C/D
// @param dstLayout : Identify whether it's A/B or C/D
// @param castOp : Identify which cast operation is used
// @param threadId : Identify which lane
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixReshape16BitElementGfx1011(
    Value *source, CooperativeMatrixElementType srcElemType, CooperativeMatrixLayout srcLayout,
    CooperativeMatrixLayout dstLayout, Value *threadId, const Twine &instName, Instruction *insertPos) {
  assert(srcElemType == CooperativeMatrixElementType::Float16 || srcElemType == CooperativeMatrixElementType::Int16);
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = nullptr;
  auto waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

  auto mapFuncX16 = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                       ArrayRef<Value *> passthroughArgs) -> Value * {
    Type *const int32Ty = builder.getInt32Ty();

    return builder.CreateIntrinsic(
        int32Ty, Intrinsic::amdgcn_permlanex16,
        {mappedArgs[0], mappedArgs[1], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };
  auto mapFunc64 = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                      ArrayRef<Value *> passthroughArgs) -> Value * {
    Type *const int32Ty = builder.getInt32Ty();

    return builder.CreateIntrinsic(int32Ty, Intrinsic::amdgcn_permlane64, {mappedArgs[0]});
  };
  if (srcLayout == CooperativeMatrixLayout::FactorMatrixLayout) { // From A/B to C/D for 16bit element
    Type *packedTy =
        (srcElemType == CooperativeMatrixElementType::Float16) ? builder.getFloatTy() : builder.getInt32Ty();
    if (dstLayout == CooperativeMatrixLayout::AccumulatorMatrixLayout) {
      unsigned vecSize = cast<FixedVectorType>(source->getType())->getNumElements();
      assert(vecSize == 8); // A/B should be 8*float16 or 8*int16
      unsigned shiftVecNums = 8;
      // wave32/wave64: lane0: {1_0:0_0 3_0:2_0....15_0:14_0}  lane15:{1_15：0_15 3_15:2_15...15_15:14_15}/lane16~lane31
      // is redundant reshape to wave32: lane0:{0_0 2_0 4_0....14_0} lane16:{1_0 3_0 5_0...15_0} wave64: lane0:{0_0 4_0
      // 8_0 12_0} lane16:{1_0 5_0 9_0 13_0} lane32:{2_0 6_0 10_0 14_0} lane48:...
      resultValue = builder.CreateBitCast(source, FixedVectorType::get(builder.getInt32Ty(), vecSize));
      if (waveSize == 64) {
        resultValue = PoisonValue::get(FixedVectorType::get(packedTy, 4));
        for (unsigned idx = 0; idx < vecSize; idx += 2) {
          Value *low = builder.CreateExtractElement(source, idx);
          Value *high = builder.CreateExtractElement(source, idx + 1);
          Value *select = builder.CreateSelect( // Select between lane0-31 and lane32-63
              builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(2)), builder.getInt32(0)), low,
              high);
          // Lane0: {1_0:0_0 5_0:4_0...} lane16=lane0  lane32: {3_0:2_0 7_0:6_0....} lane48=lane32
          resultValue = builder.CreateInsertElement(resultValue, select, idx / 2, instName);
        }
        resultValue = builder.CreateBitCast(
            resultValue,
            FixedVectorType::get(builder.getInt32Ty(), 4)); // Convert to 4*int32 for shl or and/or operation
        shiftVecNums = 4;
      }
      Value *shiftZeorValue = builder.CreateVectorSplat(shiftVecNums, builder.getInt32(0));
      Value *shift16Value = builder.CreateVectorSplat(shiftVecNums, builder.getInt32(16));

      // Wave32： lane0: {1_0:0_0 3_0:2_0....15_0:14_0}  lane16: {1_0:0_0 3_0:2_0....15_0:14_0}  =>
      // lane0: {1_0:0_0 3_0:2_0....15_0:14_0} lane16: {unused:1_0 unused:3_0....unused:15_0}
      // wave64:  lane0: {1_0:0_0 5_0:4_0...} lane16=lane0  lane32:{3_0:2_0 7_0:6_0....} lane48=lane32 =>
      // lane0:  {1_0:0_0 5_0:4_0....13_0:12_0} lane16: {unused:1_0 unused:5_0....unused:13_0} lane32:{3_0:2_0
      // 7_0:6_0....} lane48: {unused:3_0 unused:7_0....}

      // 1.Bitcast matrix to <N x i32>
      // 2.Shift right by laneGroupIndex ? 16 : 0 (you can probably do CreateLShr
      // 3.Bitcast to <N x float> if necessary:This will leave garbage in the upper 16 bits of some of the lanes,
      // but I don't think that's a problem.

      resultValue =
          builder.CreateLShr(resultValue, builder.CreateSelect(isEvenGroup, shiftZeorValue, shift16Value), instName);
      if (srcElemType == CooperativeMatrixElementType::Float16) {
        resultValue = builder.CreateBitCast(resultValue, FixedVectorType::get(builder.getFloatTy(), shiftVecNums),
                                            instName); // Bitcast to 8*bit32 for wave32 and 4*bit32 for wave64
        resultValue = builder.CreateShuffleVector(resultValue, PoisonValue::get(resultValue->getType()),
                                                  {0, 1, 2, 3, 4, 5, 6, 7});
      }
    } else if (dstLayout == CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) { // Emulation on NAVI2X
                                                                                          // from A/B to C/D on 16bit
      resultValue = PoisonValue::get(FixedVectorType::get(packedTy, 8));
      // Wave32/wave64 : lane0 : {1_0:0_0 3_0:2_0....15_0:14_0} lane16 : {1_0:0_0 3_0:2_0....15_0:14_0}
      // lane16 ~lane31 is redundant reshape to
      // Wave32/wave64 : lane0 : {1_0:0_0 5_0:4_0....13_0:12_0} lane16 : {3_0：2_0 7_0:6_0...15_0:14_0}
      source = builder.CreateBitCast(source, FixedVectorType::get(packedTy, 8));
      Value *isEvenGroup =
          builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));
      for (unsigned idx = 0; idx < 8; idx += 2) {
        Value *lowSubValue = builder.CreateExtractElement(source, idx);
        Value *highSubValue = builder.CreateExtractElement(source, idx + 1);
        Value *select = builder.CreateSelect(isEvenGroup, lowSubValue, highSubValue);
        resultValue = builder.CreateInsertElement(resultValue, select, idx / 2, instName);
      }
    } else {
      // It's unnecessary for reshape after gfx11.
      resultValue = source;
    }
  } else if (srcLayout == CooperativeMatrixLayout::AccumulatorMatrixLayout) {
    if (dstLayout == CooperativeMatrixLayout::FactorMatrixLayout) {
      // lane0----lan16----lane32-----lane48*/
      //  1x-------1y-------1m---------1n*/
      //  ==> */
      // {1y,1x}---{1y,1x}--{1n,1m}----{1n,1m},*/

      // Source now is 8*half not care wave32 or wave64
      // Zext to 8*int@wave64, the upper 16bits will not be used.
      // Permulate lane and composite the elements showns as above.
      // There will be two cases when change accumulator layout(32bit) to factor layout(16bit):
      // 1. Convert on the element: float32(fptrunc)->float16
      // 2. Reshape after MulAdd(float16*float16+float16)->Need change C/D layout to A/B layout
      // So it needs using castOp to identify which case happened.
      unsigned vecNums = 8;
      Value *matrix =
          builder.CreateShuffleVector(source, PoisonValue::get(source->getType()), {0, 1, 2, 3, 4, 5, 6, 7});

      static const unsigned LaneSelBits[2] = {0x76543210, 0xfedcba98};
      Value *swapped = builder.CreateMapToSimpleType(
          mapFuncX16,
          {
              matrix,
              matrix,
          },
          {builder.getInt32(LaneSelBits[0]), builder.getInt32(LaneSelBits[1]), builder.getFalse(), builder.getFalse()});

      Value *first = builder.CreateSelect(isEvenGroup, matrix, swapped);
      Value *second = builder.CreateSelect(isEvenGroup, swapped, matrix);

      Value *shiftValue = builder.CreateVectorSplat(vecNums, builder.getInt32(16));
      Value *maskValue = builder.CreateVectorSplat(vecNums, builder.getInt32(0xffff));

      Value *maskedFirst = builder.CreateAnd(first, maskValue);
      matrix = builder.CreateOr(maskedFirst, builder.CreateShl(second, shiftValue));

      // For wave64: step1: merge lane0+lane32 lane16+lane48
      // Each lane value: float/int32 * 4+ poison value*4
      // lane0:{1_0:0_0 5_0:4_0...} lane16:{1_0:0_0 5_0:4_0...} lane32:{3_0:2_0 7_0:6_0...} lane48{3_0:2_0 7_0:6_0...}
      // --shuffle--> lane0: {1_0:0_0 3_0:2_0 5_0:4_0....} lane16:{1_0:0_0 3_0:2_0 5_0:4_0....}  lane32: {1_0:0_0
      // 3_0:2_0 5_0:4_0....} lane48:{1_0:0_0 3_0:2_0 5_0:4_0....} For wave32: lane0: {1_0:0_0 3_0:2_0 5_0:4_0....}
      // lane16:{1_0:0_0 3_0:2_0 5_0:4_0....} lane32=lane0 lanes48=lane16

      if (waveSize == 64) {
        Value *swapped = builder.CreateMapToSimpleType(mapFunc64, matrix, {});
        Value *const laneIdLessThan32 = builder.CreateICmpULT(threadId, builder.getInt32(32));
        Value *first = builder.CreateSelect(laneIdLessThan32, matrix, swapped);
        Value *second = builder.CreateSelect(laneIdLessThan32, swapped, matrix);
        matrix = builder.CreateShuffleVector(first, second, ArrayRef<int>({0, 8, 1, 9, 2, 10, 3, 11}), instName);
      }
      // After shuffle wave64's layout is same with wave32
      if (srcElemType == CooperativeMatrixElementType::Float16) {
        matrix = builder.CreateBitCast(matrix, FixedVectorType::get(builder.getFloatTy(), 8)); //->8*f32
      }
      resultValue = matrix;
    }
  } else if (srcLayout == CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) {
    if (dstLayout == CooperativeMatrixLayout::FactorMatrixLayout) { // NAVI2X:16bit reshape C/D->A/B
      // C/D: LANE0: {1_0:0_0 5_0:4_0 9_0:8_0 13_0:12_0} LANE16:{3_0:2_0 7_0:6_0 11_0:10_0 15_0:14_0}===>
      // A/B: LANE0: {1_0:0_0 3_0:2_0 5_0:4:0....15_0:14_0}  LANE16=LANE0
      Type *packedTy =
          (srcElemType == CooperativeMatrixElementType::Float16) ? builder.getFloatTy() : builder.getInt32Ty();
      resultValue = PoisonValue::get(FixedVectorType::get(packedTy, 8));
      unsigned LaneSelBits[2] = {0x76543210, 0xfedcba98};
      Value *swapped = builder.CreateMapToSimpleType(
          mapFuncX16,
          {
              source,
              source,
          },
          {builder.getInt32(LaneSelBits[0]), builder.getInt32(LaneSelBits[1]), builder.getFalse(), builder.getFalse()});

      Value *first = builder.CreateSelect(isEvenGroup, source, swapped);
      Value *second = builder.CreateSelect(isEvenGroup, swapped, source);
      for (unsigned idx = 0; idx < 8; idx += 2) { // A/B will always be V8
        Value *firstValue = builder.CreateExtractElement(first, idx / 2);
        Value *secondValue = builder.CreateExtractElement(second, idx / 2);
        resultValue = builder.CreateInsertElement(resultValue, firstValue, idx, instName);
        resultValue = builder.CreateInsertElement(resultValue, secondValue, idx + 1, instName);
      }
    }
  } else {
    llvm_unreachable("The layout is not supported.");
  }
  return resultValue;
}

// =====================================================================================================================
// Create cooperative matrix reshape operation only for the element is int8
//
// @param source : The first operand and it should be a cooperative matrix.
// @param srcElemType : The component type of the matrix.
// @param srcLayout : Identify whether it's A/B or C/D
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixReshapeBetween8bitAnd32bitElementGfx1011(
    Value *source, CooperativeMatrixElementType srcElemType, CooperativeMatrixLayout srcLayout, const Twine &instName,
    Instruction *insertPos) {

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = nullptr;
  auto waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  Value *threadId = getLaneNumber(builder);
  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

  if (srcLayout == CooperativeMatrixLayout::FactorMatrixLayout) {
    assert(srcElemType == CooperativeMatrixElementType::Int8);
    Value *int8Value = builder.CreateBitCast(source, FixedVectorType::get(builder.getInt8Ty(), 16));
    if ((waveSize == 32) || (m_gfxIp.major < 11)) {
      Value *lowValue = builder.CreateShuffleVector(int8Value, ArrayRef<int>({0, 2, 4, 6, 8, 10, 12, 14}));
      Value *highValue = builder.CreateShuffleVector(int8Value, ArrayRef<int>({1, 3, 5, 7, 9, 11, 13, 15}));
      resultValue = builder.CreateSelect(isEvenGroup, lowValue, highValue, instName);
    } else {
      Value *lowlowValue = builder.CreateShuffleVector(int8Value, ArrayRef<int>({0, 4, 8, 12}));
      Value *lowhighValue = builder.CreateShuffleVector(int8Value, ArrayRef<int>({1, 5, 9, 13}));
      Value *highlowValue = builder.CreateShuffleVector(int8Value, ArrayRef<int>({2, 6, 10, 14}));
      Value *highhighValue = builder.CreateShuffleVector(int8Value, ArrayRef<int>({3, 7, 11, 15}));

      Value *const laneIdLessThan32 = builder.CreateICmpULT(threadId, builder.getInt32(32));
      Value *isEvenGroupLessThan32 = builder.CreateAnd(laneIdLessThan32, isEvenGroup);
      Value *isOddGroupLessThan32 = builder.CreateAnd(laneIdLessThan32, builder.CreateNot(isEvenGroup));
      Value *isEvenGroupMoreThan32 = builder.CreateAnd(builder.CreateNot(laneIdLessThan32), isEvenGroup);
      Value *isOddGroupMoreThan32 =
          builder.CreateAnd(builder.CreateNot(laneIdLessThan32), builder.CreateNot(isEvenGroup));

      resultValue = lowlowValue;
      resultValue = builder.CreateSelect(isEvenGroupLessThan32, lowlowValue, resultValue, instName);
      resultValue = builder.CreateSelect(isOddGroupLessThan32, lowhighValue, resultValue, instName);
      resultValue = builder.CreateSelect(isEvenGroupMoreThan32, highlowValue, resultValue, instName);
      resultValue = builder.CreateSelect(isOddGroupMoreThan32, highhighValue, resultValue, instName);
    }
  } else if (srcLayout == CooperativeMatrixLayout::AccumulatorMatrixLayout ||
             srcLayout == CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout) {
    //
    assert(srcElemType == CooperativeMatrixElementType::Int32 || srcElemType == CooperativeMatrixElementType::Float32);
    // unsigned vecSize = cast<FixedVectorType>(source->getType())->getNumElements();
    unsigned vecSize = 8;
    source = builder.CreateShuffleVector(source, PoisonValue::get(source->getType()), {0, 1, 2, 3, 4, 5, 6, 7});
    unsigned LaneSelBits[2] = {0x76543210, 0xfedcba98};
    auto mapFuncX16 = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                         ArrayRef<Value *> passthroughArgs) -> Value * {
      Type *const int32Ty = builder.getInt32Ty();

      return builder.CreateIntrinsic(int32Ty, Intrinsic::amdgcn_permlanex16,
                                     {mappedArgs[0], mappedArgs[1], passthroughArgs[0], passthroughArgs[1],
                                      passthroughArgs[2], passthroughArgs[3]});
    };

    Value *swapped = builder.CreateMapToSimpleType(
        mapFuncX16,
        {
            source,
            source,
        },
        {builder.getInt32(LaneSelBits[0]), builder.getInt32(LaneSelBits[1]), builder.getFalse(), builder.getFalse()});

    Value *first = builder.CreateSelect(isEvenGroup, source, swapped);
    Value *second = builder.CreateSelect(isEvenGroup, swapped, source);
    Value *afterPermValue = PoisonValue::get(FixedVectorType::get(builder.getInt8Ty(), vecSize * 2));
    for (unsigned idx = 0; idx < vecSize * 2; idx += 2) {
      Value *firstElement = builder.CreateExtractElement(first, idx / 2);
      Value *secondElement = builder.CreateExtractElement(second, idx / 2);
      afterPermValue = builder.CreateInsertElement(afterPermValue, firstElement, idx, "firstElement");
      afterPermValue = builder.CreateInsertElement(afterPermValue, secondElement, idx + 1, "secondElement");
    }
    afterPermValue = builder.CreateBitCast(afterPermValue, FixedVectorType::get(builder.getInt16Ty(), vecSize));

    if ((m_gfxIp.major == 11) && (waveSize == 64)) {
      auto mapFunc64 = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                          ArrayRef<Value *> passthroughArgs) -> Value * {
        Type *const int32Ty = builder.getInt32Ty();

        return builder.CreateIntrinsic(int32Ty, Intrinsic::amdgcn_permlane64, {mappedArgs[0]});
      };
      Value *swapped = builder.CreateMapToSimpleType(mapFunc64, afterPermValue, {});

      Value *const laneIdLessThan32 = builder.CreateICmpULT(threadId, builder.getInt32(32));
      Value *first = builder.CreateSelect(laneIdLessThan32, afterPermValue, swapped);
      Value *second = builder.CreateSelect(laneIdLessThan32, swapped, afterPermValue);
      afterPermValue = builder.CreateShuffleVector(first, second, ArrayRef<int>({0, 8, 1, 9, 2, 10, 3, 11})); // 8*int16
    }
    // bitcast: lane0 : {1_0:0_0 3_0:2_0... }(8 * int16) lane16 : {1_0 : 0_0 3_0 : 2_0...}(8 * int16) to
    //        lane0 : {3_0:2_0:1_0:0_0...}(4*int32) */
    resultValue =
        builder.CreateBitCast(afterPermValue, FixedVectorType::get(builder.getInt32Ty(), 4), "Int16V8ToInt32V4");
  } else {
    llvm_unreachable("The layout is not supported.");
  }
  return resultValue;
}

// =====================================================================================================================
// Change the 16bit layout for fconvert from f16(f32) to f32(f16)
//
// @param source : The first operand and it should be a cooperative matrix.
// @param srcLayout : Identify whether it's A/B or C/D
// @param dstLayout : Identify whether it's A/B or C/D
// @param isEvenGroup : Identify which row
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(
    Value *source, CooperativeMatrixElementType srcElemType, CooperativeMatrixElementType dstElemType,
    CooperativeMatrixLayout layout, Value *isEvenGroup, const Twine &instName, Instruction *insertPos) {
  // 1. After convert from f32->f16: change the layout from 32bit layout to 16bit layout on Accumulator on gfx10.
  // 2. Before convert from f16->f32: change the layout from 16bit layout to 32bit layout on Accumulator on gfx10

  // For 1st case:  lane0:{0_0 2_0 4_0..14_0} lane16:{1_0 3_0 5_0...15_0} lane32=lane0 lane48=lane16(8*half) ==>
  //               lane0:{1_0:0_0 5_0:4_0 ....} lane16:{3_0:2_0 7_0:6_0..} (4*float)
  // For 2nd case: lane0:{1_0:0_0 5_0:4_0 ....} lane16:{3_0:2_0 7_0:6_0..}(4*float) ==>
  //              lane0:{0_0 2_0 4_0..14_0} lane16:{1_0 3_0 5_0...15_0}(8*half)
  // From the implementation side, it's same which only exchange off-diaglog element between {2_0:0_0} and {3_0:1_0}(1st
  // case)
  //                              or {1_0:0_0} and {3_0:2_0}(2nd case)
  assert(layout == CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout ||
         layout == CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout);
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  Value *resultValue = nullptr;
  if (dstElemType == CooperativeMatrixElementType::Float16 || dstElemType == CooperativeMatrixElementType::Int16) {
    source = builder.CreateBitCast(source, FixedVectorType::get(builder.getInt32Ty(), 4));
  } else if (dstElemType == CooperativeMatrixElementType::Float32 ||
             dstElemType == CooperativeMatrixElementType::Int32) {
    source = builder.CreateBitCast(source, FixedVectorType::get(builder.getInt32Ty(), 8));
  }
  unsigned LaneSelBits[2] = {0x76543210, 0xfedcba98};
  auto mapFuncX16 = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                       ArrayRef<Value *> passthroughArgs) -> Value * {
    Type *const int32Ty = builder.getInt32Ty();

    return builder.CreateIntrinsic(
        int32Ty, Intrinsic::amdgcn_permlanex16,
        {mappedArgs[0], mappedArgs[1], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };
  Value *matrix = source;
  Value *swapped = builder.CreateMapToSimpleType(
      mapFuncX16,
      {
          matrix,
          matrix,
      },
      {builder.getInt32(LaneSelBits[0]), builder.getInt32(LaneSelBits[1]), builder.getFalse(), builder.getFalse()});

  unsigned shiftVecNums = cast<FixedVectorType>(swapped->getType())->getNumElements();
  Value *maskLowValue = builder.CreateVectorSplat(shiftVecNums, builder.getInt32(0x0000ffff));
  Value *maskHighValue = builder.CreateVectorSplat(shiftVecNums, builder.getInt32(0xffff0000));
  Value *shiftValue = builder.CreateVectorSplat(shiftVecNums, builder.getInt32(16));

  Value *maskedSourceLow = builder.CreateAnd(source, maskLowValue);
  Value *lowVal = builder.CreateSelect(isEvenGroup, maskedSourceLow,
                                       builder.CreateAnd(builder.CreateLShr(swapped, shiftValue), maskLowValue));

  Value *maskedSourceHigh = builder.CreateAnd(source, maskHighValue);
  Value *highVal = builder.CreateSelect(
      isEvenGroup, builder.CreateAnd(builder.CreateShl(swapped, shiftValue), maskHighValue), maskedSourceHigh);
  resultValue = builder.CreateOr(highVal, lowVal);

  if (srcElemType == CooperativeMatrixElementType::Float16 &&
      (dstElemType == CooperativeMatrixElementType::Float32 || dstElemType == CooperativeMatrixElementType::Int32)) {
    resultValue =
        builder.CreateBitCast(resultValue, FixedVectorType::get(builder.getHalfTy(), 16)); // 2nd case:before convert
  } else {
    resultValue =
        builder.CreateBitCast(resultValue, FixedVectorType::get(builder.getFloatTy(), 4)); // 1st case:after convert
    resultValue = builder.CreateShuffleVector(resultValue, {0, 1, 2, 3, -1, -1, -1, -1});
  }
  return resultValue;
}

// =====================================================================================================================
// Adjust the layout before reshape operation from small size type into large size type(eg:float16->float32)
//
// @param source : The first operand and it should be a cooperative matrix.
// @param srcLayout : Identify whether it's A/B or C/D
// @param dstLayout : Identify whether it's A/B or C/D
// @param srcElemType : The source component type of the matrix.
// @param dstElemType : The destination component type of the matrix.
// @param isEvenGroup : Identify which row
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixReshapeBeforeConvert(Value *source,
                                                                     CooperativeMatrixElementType srcElemType,
                                                                     CooperativeMatrixElementType dstElemType,
                                                                     CooperativeMatrixLayout srcLayout,
                                                                     CooperativeMatrixLayout dstLayout,
                                                                     const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = source;

  Value *threadId = getLaneNumber(builder);
  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

  if (srcElemType == CooperativeMatrixElementType::Float16 || srcElemType == CooperativeMatrixElementType::Int16) {
    if (srcLayout == CooperativeMatrixLayout::FactorMatrixLayout &&
        dstLayout == CooperativeMatrixLayout::AccumulatorMatrixLayout) {
      resultValue = cooperativeMatrixReshape16BitElementGfx1011(source, srcElemType, srcLayout, dstLayout, threadId,
                                                                "reshapeFactorToAcc", insertPos);
      resultValue = convCoopMatrixVecToFlatVec(builder, resultValue, srcElemType, dstLayout);
    } else if (srcLayout == CooperativeMatrixLayout::FactorMatrixLayout &&
               dstLayout == CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout) {
      resultValue = cooperativeMatrixReshape16BitElementGfx1011(
          source, srcElemType, srcLayout, CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout, threadId,
          "reshapeFactorToAcc", insertPos);
      resultValue = cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(
          resultValue, srcElemType, dstElemType, dstLayout, isEvenGroup, "beforef16tof32", insertPos);
      resultValue = convCoopMatrixVecToFlatVec(builder, resultValue, srcElemType, dstLayout);
    } else if (srcLayout == CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout &&
               dstLayout == CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout) {
      resultValue = cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(source, srcElemType, dstElemType, dstLayout,
                                                                           isEvenGroup, "beforef16tof32", insertPos);
      resultValue = convCoopMatrixVecToFlatVec(builder, resultValue, srcElemType, dstLayout);
    } else {
      llvm_unreachable("Unsupported layout!");
    }
  } else if (srcElemType == CooperativeMatrixElementType::Int8) {
    // 8bit already return the N*flatType, it's unnecessary to call convCoopMatrixVecToFlatVec
    if (srcLayout == CooperativeMatrixLayout::FactorMatrixLayout) {
      resultValue = cooperativeMatrixReshapeBetween8bitAnd32bitElementGfx1011(source, srcElemType, srcLayout,
                                                                              "reshapeFactorToAcc", insertPos);
    } else {
      // 8bit->32bit, no reshape is necessary as all elements are sorted consistently between 8bitLayout and
      // 32bitLayout.
      resultValue = convCoopMatrixVecToFlatVec(builder, resultValue, srcElemType, srcLayout);
    }
  } else {
    resultValue = convCoopMatrixVecToFlatVec(builder, resultValue, srcElemType, srcLayout);
  }
  return resultValue;
}

// =====================================================================================================================
// Adjust the layout after reshape operation from large size type into small size type(eg:float32->float16)
//
// @param source : The first operand and it should be a cooperative matrix.
// @param srcLayout : Identify whether it's A/B or C/D
// @param dstLayout : Identify whether it's A/B or C/D
// @param srcElemType : The source component type of the matrix.
// @param dstElemType : The destination component type of the matrix.
// @param isEvenGroup : Identify which row
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixReshapeAfterConvert(Value *source,
                                                                    CooperativeMatrixElementType srcElemType,
                                                                    CooperativeMatrixElementType dstElemType,
                                                                    CooperativeMatrixLayout srcLayout,
                                                                    CooperativeMatrixLayout dstLayout,
                                                                    const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = source;

  Value *threadId = getLaneNumber(builder);
  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

  if (dstElemType == CooperativeMatrixElementType::Float16 || dstElemType == CooperativeMatrixElementType::Int16) {
    if (srcLayout == CooperativeMatrixLayout::AccumulatorMatrixLayout &&
        dstLayout == CooperativeMatrixLayout::FactorMatrixLayout) {
      // It needs to convert 16bit*8 into 32bit*8(high 16bit will be unused) as
      // the input for reshape interface will be 32bit*8 keeping compatibility for reshape+muladd+reshape case.
      resultValue =
          builder.CreateShuffleVector(resultValue, PoisonValue::get(source->getType()), {0, 1, 2, 3, 4, 5, 6, 7});
      resultValue = builder.CreateBitCast(resultValue, FixedVectorType::get(builder.getInt16Ty(), 8));
      resultValue = builder.CreateZExt(resultValue, FixedVectorType::get(builder.getInt32Ty(), 8), "zext");
      resultValue = cooperativeMatrixReshape16BitElementGfx1011(resultValue, dstElemType, srcLayout, dstLayout,
                                                                threadId, "reshapeAccToFactor", insertPos);
    } else if (srcLayout == CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout &&
               dstLayout == CooperativeMatrixLayout::FactorMatrixLayout) {
      resultValue = cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(source, srcElemType, dstElemType, srcLayout,
                                                                           isEvenGroup, "afterf32tof16", insertPos);
      resultValue = cooperativeMatrixReshape16BitElementGfx1011(
          resultValue, dstElemType, CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout, dstLayout, threadId,
          "reshapeAccToFactor", insertPos);
    } else if (srcLayout == CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout &&
               dstLayout == CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) {
      resultValue = cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(source, srcElemType, dstElemType, srcLayout,
                                                                           isEvenGroup, "afterf32tof16", insertPos);
    } else {
      llvm_unreachable("Unsupported elemtype!");
    }
  } else if (dstElemType == CooperativeMatrixElementType::Int8) {
    if (dstLayout == CooperativeMatrixLayout::FactorMatrixLayout) { // gfx10/gfx11: 32bit->8bit
      resultValue = cooperativeMatrixReshapeBetween8bitAnd32bitElementGfx1011(source, srcElemType, srcLayout,
                                                                              "reshapeFactorToAcc", insertPos);
    } else {
      // 32bit->8bit, no reshape is necessary as all elements are sorted consistently between 8bitLayout and
      // 32bitLayout.
      resultValue = convFlatVecToCoopMatrixVec(builder, resultValue, dstElemType, dstLayout);
    }
  }
  return resultValue;
}

// =====================================================================================================================
// Visit "CooperativeMatrixTransposeOp" instruction
//
// @param transpose : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixTransposeOp(CooperativeMatrixTransposeOp &transpose) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&transpose);

  Value *matrix = transpose.getMatrix();
  auto elemType = transpose.getElemType();

  Value *threadId = getLaneNumber(builder);
  Value *isEvenThread = builder.CreateICmpEQ(builder.CreateAnd(threadId, builder.getInt32(1)), builder.getInt32(0));
  unsigned vecSize = cast<FixedVectorType>(matrix->getType())->getNumElements();
  unsigned vecStride, laneStride;

  auto mapFuncDpp8 = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp8, builder.getInt32Ty(),
                                   {mappedArgs[0], passthroughArgs[0]});
  };

  auto mapFuncPerm = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
    Type *const int32Ty = builder.getInt32Ty();

    return builder.CreateIntrinsic(int32Ty, Intrinsic::amdgcn_perm, {mappedArgs[0], mappedArgs[1], passthroughArgs[0]});
  };

  Value *dpp8 = builder.getInt32(1 | 0 << 3 | 3 << 6 | 2 << 9 | 5 << 12 | 4 << 15 | 7 << 18 | 6 << 21);
  Value *matrixShuffle = builder.CreateMapToSimpleType(mapFuncDpp8, matrix, {dpp8});

  if (elemType == CooperativeMatrixElementType::Int8) {
    // 1st step: {3_0:2_0:1_0:0_0} {3_1:2_1:1_1:0_1} ->
    //         {0_1:0_0:2_1:2_0} {1_1:1_0:3_1:3_0}

    Value *transValueEven =
        builder.CreateMapToSimpleType(mapFuncPerm, {matrixShuffle, matrix}, builder.getInt32(0x04000602));
    Value *transValueOdd =
        builder.CreateMapToSimpleType(mapFuncPerm, {matrixShuffle, matrix}, builder.getInt32(0x01050307));
    Value *transValue = builder.CreateSelect(isEvenThread, transValueEven, transValueOdd);

    // 2nd step
    // 0_1:0_0:2_1:2_0----1_1:1_0:3_1:3_0-----0_3:0_2:2_3:2_2----1_3:1_2:3_3:3_2 ==>
    // 0_3:0_2:0_1:0_0	1_3:1_2:1_1:1_0	    2_3:2_2:2_1:2_0	   3_3:3_2:3_1:3_0

    dpp8 = builder.getInt32(2 | 3 << 3 | 0 << 6 | 1 << 9 | 6 << 12 | 7 << 15 | 4 << 18 | 5 << 21);

    Value *transValueShuffle = builder.CreateMapToSimpleType(mapFuncDpp8, transValue, {dpp8});
    Value *srclowlane = builder.CreateICmpEQ(builder.CreateAnd(threadId, builder.getInt32(2)), builder.getInt32(0));

    Value *matrixSlow =
        builder.CreateMapToSimpleType(mapFuncPerm, {transValueShuffle, transValue}, builder.getInt32(0x07060302));
    Value *matrixHigh =
        builder.CreateMapToSimpleType(mapFuncPerm, {transValueShuffle, transValue}, builder.getInt32(0x01000504));
    matrix = builder.CreateSelect(srclowlane, matrixSlow, matrixHigh);

    vecStride = 1;
    laneStride = 4;
  } else if (elemType == CooperativeMatrixElementType::Int16 || elemType == CooperativeMatrixElementType::Float16) {
    // lane0:{1_0, 0_0} lane1:{1_1,0_1} -> lane0: {0_1, 0_0} lane1:{1_1, 1_0}
    matrix = builder.CreateBitCast(matrix, FixedVectorType::get(builder.getInt32Ty(), vecSize));
    matrixShuffle = builder.CreateBitCast(matrixShuffle, FixedVectorType::get(builder.getInt32Ty(), vecSize));
    Value *shiftValue = builder.CreateVectorSplat(vecSize, builder.getInt32(16));
    Value *highmaskValue = builder.CreateVectorSplat(vecSize, builder.getInt32(0xFFFF0000));
    Value *lowmaskValue = builder.CreateVectorSplat(vecSize, builder.getInt32(0x0000FFFF));

    Value *maskedMatrixHigh = builder.CreateAnd(matrix, highmaskValue);
    Value *high = builder.CreateSelect(isEvenThread, builder.CreateShl(matrixShuffle, shiftValue), maskedMatrixHigh);
    Value *maskedMatrixLow = builder.CreateAnd(matrix, lowmaskValue);
    Value *low = builder.CreateSelect(isEvenThread, maskedMatrixLow, builder.CreateLShr(matrixShuffle, shiftValue));
    matrix = builder.CreateOr(high, low);
    if (elemType == CooperativeMatrixElementType::Float16) {
      matrix = builder.CreateBitCast(matrix, FixedVectorType::get(builder.getFloatTy(), vecSize));
    }
    vecStride = 1;
    laneStride = 2;
  } else {
    llvm_unreachable("Element type is not supported.");
  }

  // lane0/V0: {0_0,0_1}; V1: {2_0,2_1} lane2/V0:{0_2,0_3} V1:{2_2,2_3} ==>
  // lane0/V0: {0_0,0_1}; V1: {0_2,0_3} lane2/V0:{2_0,2_1} V1:{2_2,2_3}
  Value *resultValue = transposeCooperativeMatrixRecursively(matrix, vecStride, laneStride, threadId, builder);
  m_coopMatrixCalls.push_back(&transpose);
  transpose.replaceAllUsesWith(resultValue);
}

// =====================================================================================================================
// Create cooperative matrix transpose
// @param matrix : The first operand and it should be a cooperative matrix.
// @param vecStride : Identify stride in element vector when transpose block size
// @param laneStride : Identify stride in lane when transpose block size
// @param threadId : Current threadId.
// @param builder : The IR builder to create and insert IR instruction
Value *LowerCooperativeMatrix::transposeCooperativeMatrixRecursively(llvm::Value *matrix, unsigned vecStride,
                                                                     unsigned laneStride, Value *threadId,
                                                                     BuilderBase &builder) {
  unsigned vgprNums = cast<FixedVectorType>(matrix->getType())->getNumElements();
  if (vecStride >= vgprNums) {
    return matrix;
  }

  DppCtrl dppCtrl = DppCtrl::DppQuadPerm0000;
  switch (laneStride) {
  case 1:
    dppCtrl = DppCtrl::DppRowXmask1;
    break;
  case 2:
    dppCtrl = DppCtrl::DppRowXmask2;
    break;
  case 4:
    dppCtrl = DppCtrl::DppRowXmask4;
    break;
  case 8:
    dppCtrl = DppCtrl::DppRowXmask8;
    break;
  default:
    llvm_unreachable("The stride is not correct!");
  }

  auto mapFuncdppmove = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                           ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(
        Intrinsic::amdgcn_mov_dpp, builder.getInt32Ty(),
        {mappedArgs[0], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };

  Value *transResultValue = PoisonValue::get(matrix->getType());
  Value *replaceLaneValue =
      builder.CreateMapToSimpleType(mapFuncdppmove, matrix,
                                    {builder.getInt32(static_cast<unsigned>(dppCtrl)), builder.getInt32(15),
                                     builder.getInt32(15), builder.getTrue()});

  Value *swapFlag = builder.CreateICmpNE(builder.CreateAnd(threadId, laneStride), builder.getInt32(0));
  Value *inverseSwapFlag = builder.CreateNot(swapFlag);

  for (int index = 0; index < vgprNums; ++index) {
    Value *srcValue = builder.CreateExtractElement(matrix, index);
    unsigned replaceValueIndex = index ^ vecStride;
    Value *replaceValue = builder.CreateExtractElement(replaceLaneValue, replaceValueIndex);
    // if (i & n) == 0:
    //    dst = swapFlag ? tmp : src
    // else:
    //    dst = inverseSwapFlag ? tmp : src
    Value *dst;
    if ((index & vecStride) == 0) {
      dst = builder.CreateSelect(swapFlag, replaceValue, srcValue);
    } else {
      dst = builder.CreateSelect(inverseSwapFlag, replaceValue, srcValue);
    }
    transResultValue = builder.CreateInsertElement(transResultValue, dst, index);
  }

  vecStride = vecStride << 1;
  laneStride = laneStride << 1;

  transResultValue = transposeCooperativeMatrixRecursively(transResultValue, vecStride, laneStride, threadId, builder);
  return transResultValue;
}

// =====================================================================================================================
// Visit "CooperativeMatrixMulAddOp" instruction
//
// @param muladd : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixMulAddOp(CooperativeMatrixMulAddOp &muladd) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&muladd);

  Value *matrixA = muladd.getMatrixA();
  Value *matrixB = muladd.getMatrixB();
  Value *matrixC = muladd.getMatrixC();
  auto factorElemType = muladd.getFactorElemType();
  auto accumElemType = muladd.getAccuElemType();
  bool isSignedA = muladd.getIsSignedA();
  bool isSignedB = muladd.getIsSignedB();
  bool isSatOrOpsel = muladd.getIsSatOrOpsel();
  StringRef instName = muladd.getName();

  if (m_gfxIp.major >= 11) {
    // Gfx11:
    // wave64:
    // declare <4 x float> @llvm.amdgcn.wmma.f32.16x16x16.f16(<16 x half>, <16 x half>, <4 x float>)
    // declare <4 x float> @llvm.amdgcn.wmma.f32.16x16x16.bf16(<16 x i16>, <16 x i16>, <4 x float>)
    // declare <8 x half> @llvm.amdgcn.wmma.f16.16x16x16.f16(<16 x half>, <16 x half>, <8 x half>, i1 immarg)
    // declare <8 x i16> @llvm.amdgcn.wmma.bf16.16x16x16.bf16(<16 x i16>, <16 x i16>, <8 x i16>, i1 immarg)
    // declare <4 x i32> @llvm.amdgcn.wmma.i32.16x16x16.iu8(i1 immarg, <4 x i32>, i1 immarg, <4 x i32>, <4 x i32>, i1
    // immarg) declare <4 x i32> @llvm.amdgcn.wmma.i32.16x16x16.iu4(i1 immarg, <2 x i32>, i1 immarg, <2 x i32>, <4 x
    // i32>, i1 immarg)
    // wave32:
    // declare <8 x float> @llvm.amdgcn.wmma.f32.16x16x16.f16(<16 x half>, <16 x half> , <8 x float>)
    // declare <8 x float> @llvm.amdgcn.wmma.f32.16x16x16.bf16(<16 x i16>, <16 x i16> , <8 x float>)
    // declare <16 x half> @llvm.amdgcn.wmma.f16.16x16x16.f16(<16 x half>, <16 x half> , <16 x half>, i1 immarg)
    // declare <16 x i16> @llvm.amdgcn.wmma.bf16.16x16x16.bf16(<16 x i16>, <16 x i16> , <16 x i16>, i1 immarg)
    // declare <8 x i32> @llvm.amdgcn.wmma.i32.16x16x16.iu8(i1 immarg, <4 x i32>, i1 immarg, <4 x i32> , <8 x i32>, i1
    // immarg) declare <8 x i32> @llvm.amdgcn.wmma.i32.16x16x16.iu4(i1 immarg, <2 x i32>, i1 immarg, <2 x i32> , <8 x
    // i32>, i1 immarg)
    Value *matrixD;
    unsigned waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);

    if (factorElemType == CooperativeMatrixElementType::Float16 ||
        factorElemType == CooperativeMatrixElementType::Int16) {
      unsigned factorFlatElemNum = 0;
      { factorFlatElemNum = 16; }
      Type *factorType =
          FixedVectorType::get(builder.transCooperativeMatrixElementType(factorElemType), factorFlatElemNum);
      matrixA = builder.CreateBitCast(matrixA, factorType);
      matrixB = builder.CreateBitCast(matrixB, factorType);
    } else if (factorElemType == CooperativeMatrixElementType::Int8) {
    } else {
      llvm_unreachable("Factor element type is not supported!");
    }

    if (accumElemType == CooperativeMatrixElementType::Float32 ||
        accumElemType == CooperativeMatrixElementType::Int32) {
      matrixC =
          waveSize == 64 ? builder.CreateShuffleVector(matrixC, ArrayRef<int>({0, 1, 2, 3}), "shuffleVector") : matrixC;
    } else if (accumElemType == CooperativeMatrixElementType::Float16 ||
               accumElemType == CooperativeMatrixElementType::Int16) {
      {
        matrixC = waveSize == 64 ? builder.CreateShuffleVector(matrixC, ArrayRef<int>({0, 1, 2, 3}), "shuffleVector")
                                 : matrixC;
      }
      unsigned matrixLength = cast<FixedVectorType>(matrixC->getType())->getNumElements();
      Type *accumType = FixedVectorType::get(builder.getHalfTy(), matrixLength * 2);
      matrixC = builder.CreateBitCast(matrixC, accumType);
    } else {
      llvm_unreachable("Accumulator element type is not supported!");
    }

    if (factorElemType == CooperativeMatrixElementType::Float16 &&
        accumElemType == CooperativeMatrixElementType::Float32) {
      matrixD = builder.CreateIntrinsic(matrixC->getType(), Intrinsic::amdgcn_wmma_f32_16x16x16_f16,
                                        {matrixA, matrixB, matrixC}, nullptr, instName);

    } else if (factorElemType == CooperativeMatrixElementType::Int8 &&
               accumElemType == CooperativeMatrixElementType::Int32) {
      matrixD = builder.CreateIntrinsic(matrixC->getType(), Intrinsic::amdgcn_wmma_i32_16x16x16_iu8,
                                        {builder.getInt1(isSignedA), matrixA, builder.getInt1(isSignedB), matrixB,
                                         matrixC, builder.getInt1(isSatOrOpsel)},
                                        nullptr, instName);

    } else if (factorElemType == CooperativeMatrixElementType::Float16 &&
               accumElemType == CooperativeMatrixElementType::Float16) {
      // Matrix convert to match intrinsic arguments: Wave32: float32*v8->half*v16
      // Wave64: float32*v4->half*v8
      bool isTied = muladd.getIsTied();
      auto intrinsic = Intrinsic::amdgcn_wmma_f16_16x16x16_f16;
      if (isTied)
#if defined(LLVM_MAIN_REVISION) && LLVM_MAIN_REVISION < 479080
        llvm_unreachable("Tied intrinsics not implemented");
#else
        intrinsic = Intrinsic::amdgcn_wmma_f16_16x16x16_f16_tied;
#endif
      matrixD = builder.CreateIntrinsic(matrixC->getType(), intrinsic,
                                        {matrixA, matrixB, matrixC, builder.getInt1(isSatOrOpsel)}, nullptr, instName);
    } else {
      llvm_unreachable("The accumulator type is not supported.");
    }

    if (accumElemType == CooperativeMatrixElementType::Float16 ||
        accumElemType == CooperativeMatrixElementType::Int16) {
      unsigned coopVeclength = cast<FixedVectorType>(matrixD->getType())->getNumElements();
      Type *wordTy = builder.transCooperativeMatrixElementType(accumElemType)->isIntOrIntVectorTy()
                         ? builder.getInt32Ty()
                         : builder.getFloatTy();
      matrixD = builder.CreateBitCast(matrixD, FixedVectorType::get(wordTy, coopVeclength / 2));
      {
        matrixD = waveSize == 64 ? builder.CreateShuffleVector(matrixD, PoisonValue::get(matrixD->getType()),
                                                               ArrayRef<int>{0, 1, 2, 3, 4, 5, 6, 7})
                                 : matrixD;
      }
    } else {
      matrixD = waveSize == 64 ? builder.CreateShuffleVector(matrixD, PoisonValue::get(matrixD->getType()),
                                                             ArrayRef<int>{0, 1, 2, 3, 4, 5, 6, 7})
                               : matrixD;
    }
    m_coopMatrixCalls.push_back(&muladd);
    muladd.replaceAllUsesWith(matrixD);
    return;
  }

  // Emulator on NAVI2X
  Type *packedTy =
      (factorElemType == CooperativeMatrixElementType::Float16) ? builder.getFloatTy() : builder.getInt32Ty();
  Value *dotProductValue;

  Value *threadId = getLaneNumber(builder);
  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

  unsigned flags = (isSignedB << 1) | isSignedA;
  auto mapFuncReadLane = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                            ArrayRef<Value *> passthroughArgs) -> Value * {
    Type *const int32Ty = builder.getInt32Ty();

    return builder.CreateIntrinsic(int32Ty, Intrinsic::amdgcn_readlane, {mappedArgs[0], passthroughArgs[0]});
  };

  // matrixC is not reshaped for gfx10
  if (accumElemType == CooperativeMatrixElementType::Float32 || accumElemType == CooperativeMatrixElementType::Int32) {
    dotProductValue = PoisonValue::get(FixedVectorType::get(packedTy, 8));
    for (unsigned idxc = 0; idxc < 8; ++idxc) {
      Value *rowlowgroup = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc * 2));
      Value *rowhighgroup = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc * 2 + 1));
      Value *rowData = builder.CreateSelect(isEvenGroup, rowlowgroup, rowhighgroup);
      Value *mulAB;
      Value *initAccumulator = builder.CreateExtractElement(matrixC, idxc);
      if (factorElemType == CooperativeMatrixElementType::Float16) {
        mulAB = createDotProductFp16Fp32(rowData, matrixB, initAccumulator, isSatOrOpsel, instName, &muladd);
      } else if (factorElemType == CooperativeMatrixElementType::Int16) {
        mulAB = createDotProductInt16Int32(rowData, matrixB, initAccumulator, flags, isSatOrOpsel, instName, &muladd);
      } else if (factorElemType == CooperativeMatrixElementType::Int8) {
        mulAB = createDotProductInt8Int32(rowData, matrixB, initAccumulator, flags, isSatOrOpsel, instName, &muladd);
      } else {
        llvm_unreachable("Unsupported element type!");
      }
      dotProductValue = builder.CreateInsertElement(dotProductValue, mulAB, idxc);
    }
  } else if (accumElemType == CooperativeMatrixElementType::Int16 ||
             accumElemType == CooperativeMatrixElementType::Float16) {
    dotProductValue =
        PoisonValue::get(FixedVectorType::get(builder.transCooperativeMatrixElementType(accumElemType), 8));
    // For gfx10, A*B:8*float32->16*half  C: no reshape for 16bit, still 16*half
    Value *colData =
        convCoopMatrixVecToFlatVec(builder, matrixB, factorElemType, CooperativeMatrixLayout::FactorMatrixLayout);
    matrixC = convCoopMatrixVecToFlatVec(builder, matrixC, accumElemType,
                                         CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout);

    for (unsigned idxc = 0, accIdx = 0; idxc < 16; idxc += 4, accIdx += 2) {
      Value *rowData1Low = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc));
      Value *rowData2Low = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc + 1));
      Value *rowData1High = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc + 2));
      Value *rowData2High = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc + 3));

      Value *rowData1 = builder.CreateSelect(isEvenGroup, rowData1Low, rowData1High);
      Value *rowData2 = builder.CreateSelect(isEvenGroup, rowData2Low, rowData2High);

      rowData1 =
          convCoopMatrixVecToFlatVec(builder, rowData1, factorElemType, CooperativeMatrixLayout::FactorMatrixLayout);
      rowData2 =
          convCoopMatrixVecToFlatVec(builder, rowData2, factorElemType, CooperativeMatrixLayout::FactorMatrixLayout);

      Value *mulAB1;
      Value *mulAB2;
      Value *accumulator1 = builder.CreateExtractElement(matrixC, accIdx);
      Value *accumulator2 = builder.CreateExtractElement(matrixC, accIdx + 1);

      if (accumElemType == CooperativeMatrixElementType::Float16) {
        mulAB1 = createDotProductFp16Fp16(rowData1, colData, accumulator1, isSatOrOpsel, instName, &muladd);
        mulAB2 = createDotProductFp16Fp16(rowData2, colData, accumulator2, isSatOrOpsel, instName, &muladd);
      } else {
        mulAB1 = createDotProductInt(rowData1, colData, accumulator1, flags, isSatOrOpsel, instName, &muladd);
        mulAB2 = createDotProductInt(rowData2, colData, accumulator2, flags, isSatOrOpsel, instName, &muladd);
      }
      dotProductValue = builder.CreateInsertElement(dotProductValue, mulAB1, accIdx);
      dotProductValue = builder.CreateInsertElement(dotProductValue, mulAB2, accIdx + 1);
    }

    dotProductValue = convFlatVecToCoopMatrixVec(builder, dotProductValue, accumElemType,
                                                 CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout);
  } else {
    llvm_unreachable("The accumulator type is not supported.");
  }
  m_coopMatrixCalls.push_back(&muladd);
  muladd.replaceAllUsesWith(dotProductValue);
}

// =====================================================================================================================
// Create scalar from dot product of scalar or vector FP type. (The dot product of two scalars is their product.)
// The two vectors must be the same floating point scalar/vector type.
// Returns a value whose type is the element type of the vectors.
//
// @param vector1 : The float vector 1
// @param vector2 : The float vector 2
// @param initAccumulator : Initial accumulator
// @param isSat:  SaturatingAccumulation for calculation
// @param instName : Name to give instruction(s)
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::createDotProductFp16Fp32(Value *const vector1, Value *const vector2,
                                                        Value *const initAccumulator, bool isSat, const Twine &instName,
                                                        Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  // Dot instructions are not available on gfx1010
  const bool emulateDot = m_gfxIp.isGfx(10, 1) && m_gfxIp.stepping == 0;
  const unsigned compCount = cast<FixedVectorType>(vector1->getType())->getNumElements();
  Value *scalar = initAccumulator;
  auto intrinsicDot = Intrinsic::amdgcn_fdot2;
  for (unsigned i = 0; i < compCount; ++i) {
    Value *input1 = builder.CreateExtractElement(vector1, i);
    input1 = builder.CreateBitCast(input1, FixedVectorType::get(builder.getHalfTy(), 2));
    Value *input2 = builder.CreateExtractElement(vector2, i);
    input2 = builder.CreateBitCast(input2, FixedVectorType::get(builder.getHalfTy(), 2));
    if (emulateDot) {
      Value *input1Fp32 = builder.CreateFPCast(input1, FixedVectorType::get(builder.getFloatTy(), 2));
      Value *input2Fp32 = builder.CreateFPCast(input2, FixedVectorType::get(builder.getFloatTy(), 2));
      for (unsigned j = 0; j < 2; ++j) {
        Value *lhs = builder.CreateExtractElement(input1Fp32, j);
        Value *rhs = builder.CreateExtractElement(input2Fp32, j);
        scalar = builder.CreateIntrinsic(Intrinsic::fmuladd, lhs->getType(), {lhs, rhs, scalar});
      }
    } else {
      scalar = builder.CreateIntrinsic(intrinsicDot, {}, {input1, input2, scalar, builder.getInt1(isSat)}, nullptr,
                                       instName);
    }
  }
  scalar->setName(instName);
  return scalar;
}

// =====================================================================================================================
// Create scalar from dot product of scalar or vector FP type. (The dot product of two scalars is their product.)
//
// @param vector1 : The float vector 1
// @param vector2 : The float vector 2
// @param initAccumulator : Initial accumulator
// @param isSat:  SaturatingAccumulation for calculation
// @param instName : Name to give instruction(s)
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::createDotProductFp16Fp16(Value *const vector1, Value *const vector2,
                                                        Value *const initAccumulator, bool isSat, const Twine &instName,
                                                        Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  Value *product = builder.CreateFMul(vector1, vector2);
  if (!isa<VectorType>(product->getType()))
    return product;

  const unsigned compCount = cast<FixedVectorType>(product->getType())->getNumElements();
  Value *scalar = initAccumulator;

  for (unsigned i = 0; i < compCount; ++i)
    scalar = builder.CreateFAdd(scalar, builder.CreateExtractElement(product, i));

  scalar->setName(instName);
  return scalar;
}

// =====================================================================================================================
// Create code to calculate the dot product of two integer vectors, with optional accumulator, using hardware support
// where available.
// Use a value of 0 for no accumulation and the value type is consistent with the result type. The result is saturated
// if there is an accumulator. The component type of input vectors can have 8-bit/16-bit/32-bit and i32/i16/i8 result.
//
// @param vector1 : The integer vector 1
// @param vector2 : The integer vector 2
// @param accumulator : The accumulator to the scalar of dot product
// @param flags : Bit 0 is "first vector is signed" and bit 1 is "second vector is signed"
// @param isSat:  SaturatingAccumulation for calculation
// @param instName : Name to give instruction(s)
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::createDotProductInt8Int32(Value *vector1, Value *vector2, Value *accumulator,
                                                         unsigned flags, bool isSat, const Twine &instName,
                                                         Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  // Dot instructions are not available on gfx1010
  const bool emulateDot = m_gfxIp.isGfx(10, 1) && m_gfxIp.stepping == 0;
  const bool isSigned = (flags & lgc::Builder::FirstVectorSigned);
  auto intrinsicDot = isSigned ? Intrinsic::amdgcn_sdot4 : Intrinsic::amdgcn_udot4;

  Value *scalar = builder.getInt32(0);
  const unsigned compCount = cast<FixedVectorType>(vector1->getType())->getNumElements();
  for (unsigned i = 0; i < compCount; ++i) {
    Value *input1 = builder.CreateExtractElement(vector1, i);
    Value *input2 = builder.CreateExtractElement(vector2, i);
    if (emulateDot) {
      input1 = builder.CreateBitCast(input1, FixedVectorType::get(builder.getInt8Ty(), 4));
      input2 = builder.CreateBitCast(input2, FixedVectorType::get(builder.getInt8Ty(), 4));
      scalar = createDotProductInt(input1, input2, scalar, flags, isSat, instName, insertPos);
    } else {
      scalar = builder.CreateIntrinsic(intrinsicDot, {}, {input1, input2, scalar, builder.getInt1(false)}, nullptr,
                                       instName);
    }
  }

  // Always use sadd_sat here as uint32@C is not supported.
  scalar = builder.CreateSExt(scalar, builder.getInt32Ty());
  if (isSat) {
    scalar = builder.CreateBinaryIntrinsic(Intrinsic::sadd_sat, scalar, accumulator, nullptr, instName);
  } else {
    scalar = builder.CreateAdd(scalar, accumulator, instName);
  }
  scalar->setName(instName);
  return scalar;
}

// =====================================================================================================================
// Create code to calculate the dot product of two integer vectors, with optional accumulator
//
// @param vector1 : The integer vector 1
// @param vector2 : The integer vector 2
// @param accumulator : The accumulator to the scalar of dot product
// @param flags : Bit 0 is "first vector is signed" and bit 1 is "second vector is signed"
// @param isSat:  SaturatingAccumulation for calculation
// @param instName : Name to give instruction(s)
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::createDotProductInt16Int32(Value *vector1, Value *vector2, Value *accumulator,
                                                          unsigned flags, bool isSat, const Twine &instName,
                                                          Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  // Dot instructions are not available on gfx1010
  const bool emulateDot = m_gfxIp.isGfx(10, 1) && m_gfxIp.stepping == 0;
  const bool isSigned = (flags & lgc::Builder::FirstVectorSigned);
  auto intrinsicDot = isSigned ? Intrinsic::amdgcn_sdot2 : Intrinsic::amdgcn_udot2;

  Value *scalar = accumulator;
  const unsigned compCount = cast<FixedVectorType>(vector1->getType())->getNumElements();
  for (unsigned i = 0; i < compCount; ++i) {
    Value *input1 = builder.CreateExtractElement(vector1, i);
    input1 = builder.CreateBitCast(input1, FixedVectorType::get(builder.getInt16Ty(), 2));
    Value *input2 = builder.CreateExtractElement(vector2, i);
    input2 = builder.CreateBitCast(input2, FixedVectorType::get(builder.getInt16Ty(), 2));
    if (emulateDot) {
      scalar = createDotProductInt(input1, input2, scalar, flags, isSat, instName, insertPos);
    } else {
      scalar = builder.CreateIntrinsic(intrinsicDot, {}, {input1, input2, scalar, builder.getInt1(isSat)}, nullptr,
                                       instName);
    }
  }
  scalar->setName(instName);
  return scalar;
}

// =====================================================================================================================
// Create code to calculate the dot product of two integer vectors, with optional accumulator
//
// @param vector1 : The integer vector 1
// @param vector2 : The integer vector 2
// @param accumulator : The accumulator to the scalar of dot product
// @param flags : Bit 0 is "first vector is signed" and bit 1 is "second vector is signed"
// @param isSat:  SaturatingAccumulation for calculation
// @param instName : Name to give instruction(s)
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::createDotProductInt(Value *vector1, Value *vector2, Value *accumulator, unsigned flags,
                                                   bool isSat, const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Type *inputTy = vector1->getType();
  assert(inputTy->isVectorTy() && inputTy->getScalarType()->isIntegerTy());

  const unsigned compCount = cast<FixedVectorType>(inputTy)->getNumElements();
  Type *outputTy = accumulator->getType();
  // The component of Vector 2 can be signed or unsigned
  const bool isSigned = (flags & lgc::Builder::FirstVectorSigned);
  // The mixed signed/unsigned is that component of Vector 1 is treated as signed and component of Vector 2 is treated
  // as unsigned.
  const bool isMixed = (flags == lgc::Builder::FirstVectorSigned);

  const auto outputSizeInBits = outputTy->getScalarSizeInBits();
  const auto compSizeInBits = inputTy->getScalarSizeInBits();
  Type *targetTy = compSizeInBits * 2 >= outputSizeInBits ? builder.getIntNTy(outputSizeInBits * 2) : outputTy;
  const auto targetSizeInBits = targetTy->getScalarSizeInBits();
  assert(targetSizeInBits <= 64);
  // Emulate dot product with no HW support cases
  Value *scalar = builder.getIntN(targetSizeInBits, 0);
  for (unsigned elemIdx = 0; elemIdx < compCount; ++elemIdx) {
    Value *elem1 = builder.CreateExtractElement(vector1, elemIdx);
    elem1 = isSigned ? builder.CreateSExt(elem1, targetTy) : builder.CreateZExt(elem1, targetTy);
    Value *elem2 = builder.CreateExtractElement(vector2, elemIdx);
    elem2 = (isSigned && !isMixed) ? builder.CreateSExt(elem2, targetTy) : builder.CreateZExt(elem2, targetTy);
    Value *product = builder.CreateMul(elem1, elem2);
    scalar = builder.CreateAdd(product, scalar);
  }

  scalar = builder.CreateTrunc(scalar, outputTy);
  accumulator = builder.CreateTrunc(accumulator, outputTy);
  Intrinsic::ID addIntrinsic = isSigned ? Intrinsic::sadd_sat : Intrinsic::uadd_sat;
  scalar = builder.CreateBinaryIntrinsic(addIntrinsic, scalar, accumulator, nullptr, instName);

  auto unsignedMax = (2ULL << (targetSizeInBits - 1)) - 1;
  auto signedMax = unsignedMax >> 1;
  auto signedMin = -1ULL - signedMax;

  Value *minimum = nullptr, *maximum = nullptr;
  Value *isUnderflow = nullptr, *isOverflow = nullptr;
  if (isSigned) {
    scalar = builder.CreateSExt(scalar, targetTy);
    minimum = ConstantInt::getSigned(targetTy, signedMin);
    maximum = ConstantInt::getSigned(targetTy, signedMax);
    isUnderflow = builder.CreateICmpSLT(scalar, minimum);
    isOverflow = builder.CreateICmpSGT(scalar, maximum);
  } else {
    scalar = builder.CreateZExt(scalar, targetTy);
    minimum = builder.getIntN(targetSizeInBits, 0);
    maximum = builder.getIntN(targetSizeInBits, unsignedMax);
    isUnderflow = builder.CreateICmpULT(scalar, minimum);
    isOverflow = builder.CreateICmpUGT(scalar, maximum);
  }
  scalar = builder.CreateSelect(isUnderflow, minimum, scalar);
  scalar = builder.CreateSelect(isOverflow, maximum, scalar);
  scalar = builder.CreateTrunc(scalar, outputTy);

  scalar->setName(instName);
  return scalar;
}

// =====================================================================================================================
// Visit "CooperativeMatrixPackOp" instruction
//
// @param pack: The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixPackOp(CooperativeMatrixPackOp &pack) {
  Value *matrixCLo = pack.getMatrixCLo();
  Value *matrixCHi = pack.getMatrixCHi();

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&pack);

  static const int shuffleIndices[] = {0, 16, 2, 18, 4, 20, 6, 22, 8, 24, 10, 26, 12, 28, 14, 30};

  const auto halfVecTy = FixedVectorType::get(builder.getHalfTy(), 16);
  matrixCLo = builder.CreateBitCast(matrixCLo, halfVecTy);
  matrixCHi = builder.CreateBitCast(matrixCHi, halfVecTy);

  auto result = builder.CreateShuffleVector(matrixCLo, matrixCHi, shuffleIndices);

  Value *packValue = builder.CreateBitCast(result, FixedVectorType::get(builder.getFloatTy(), 8));
  m_coopMatrixCalls.push_back(&pack);
  pack.replaceAllUsesWith(packValue);
}

// =====================================================================================================================
// Visit "CooperativeMatrixUnPackOp" instruction
//
// @param unpack: The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeMatrixUnPackOp(CooperativeMatrixUnPackOp &unpack) {
  Value *packedMatrix = unpack.getPackedMatrix();
  bool high = unpack.getGetUpperHalf();

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&unpack);

  static const int shuffleIndicesLo[] = {0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 10, -1, 12, -1, 14, -1};
  static const int shuffleIndicesHi[] = {1, -1, 3, -1, 5, -1, 7, -1, 9, -1, 11, -1, 13, -1, 15, -1};

  const auto halfVecTy = FixedVectorType::get(builder.getHalfTy(), 16);
  auto matrixPackedCast = builder.CreateBitCast(packedMatrix, halfVecTy);
  auto matrixUnpacked = builder.CreateShuffleVector(matrixPackedCast, high ? shuffleIndicesHi : shuffleIndicesLo);

  Value *unpackValue = builder.CreateBitCast(matrixUnpacked, FixedVectorType::get(builder.getFloatTy(), 8));
  m_coopMatrixCalls.push_back(&unpack);
  unpack.replaceAllUsesWith(unpackValue);
}

// =====================================================================================================================
// Get lane id.
// @param builder : The IR builder to create and insert IR instruction
Value *LowerCooperativeMatrix::getLaneNumber(BuilderBase &builder) {
  Value *result = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});
  if (m_pipelineState->getShaderWaveSize(m_shaderStage) == 64)
    result = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), result});
  return result;
}

// =====================================================================================================================
// Visit "CooperativeRowAccLoadOp" instruction
//
// @param inst : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeRowAccLoadOp(CooperativeRowAccLoadOp &load) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&load);

  auto dataPtr = load.getPointer();
  auto stride = load.getStride();
  auto elemType = load.getElemType();
  auto memoryAccess = load.getMemoryAccess();

  assert(builder.transCooperativeMatrixElementType(elemType) == load.getType());

  // Calc element offset in memory
  Type *elemTy = builder.transCooperativeMatrixElementType(elemType);
  const unsigned dataBitwidth = elemTy->getScalarSizeInBits();
  const unsigned addrSpace = dataPtr->getType()->getPointerAddressSpace();
  assert(addrSpace == ADDR_SPACE_LOCAL || addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER || addrSpace == ADDR_SPACE_GLOBAL);

  stride = builder.CreateExactSDiv(stride, builder.getInt32(dataBitwidth / 8));

  // calc memoryAccess
  bool isVolatile = (unsigned)(memoryAccess) & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessVolatileMask);
  bool isCoherent = (unsigned)(memoryAccess) & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessCoherentMask);
  bool isTemporal = (unsigned)(memoryAccess) & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessTemporalMask);

  Value *threadId = getLaneNumber(builder);
  Value *colOffsetPerLane = builder.CreateSRem(threadId, builder.getInt32(16));
  Value *offset = builder.CreateMul(colOffsetPerLane, stride);

  Value *elemPtr = builder.CreateGEP(elemTy, dataPtr, offset);
  Value *elemVal = builder.CreateLoad(elemTy, elemPtr, isVolatile);
  if (isCoherent && !(addrSpace == ADDR_SPACE_LOCAL && dataBitwidth < 32))
    cast<LoadInst>(elemVal)->setAtomic(AtomicOrdering::Unordered);
  if (isTemporal)
    cast<LoadInst>(elemVal)->setMetadata(LLVMContext::MD_nontemporal, MDNode::get(builder.getContext(), {}));

  m_coopRowAccCalls.push_back(&load);
  load.replaceAllUsesWith(elemVal);
}

// =====================================================================================================================
// Visit "CooperativeRowAccStoreOp" instruction
//
// @param inst : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeRowAccStoreOp(CooperativeRowAccStoreOp &store) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&store);

  auto dataPtr = store.getPointer();
  auto stride = store.getStride();
  auto elemType = store.getElemType();
  auto memoryAccess = store.getMemoryAccess();
  auto data = store.getData();

  assert(builder.transCooperativeMatrixElementType(elemType) == data->getType());

  // Calc element offset in memory
  Type *elemTy = builder.transCooperativeMatrixElementType(elemType);
  const unsigned dataBitwidth = elemTy->getScalarSizeInBits();
  const unsigned addrSpace = dataPtr->getType()->getPointerAddressSpace();
  assert(addrSpace == ADDR_SPACE_LOCAL || addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER || addrSpace == ADDR_SPACE_GLOBAL);

  stride = builder.CreateExactSDiv(stride, builder.getInt32(dataBitwidth / 8));

  // calc memoryAccess
  bool isVolatile = (unsigned)(memoryAccess) & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessVolatileMask);
  bool isCoherent = (unsigned)(memoryAccess) & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessCoherentMask);
  bool isTemporal = (unsigned)(memoryAccess) & (unsigned)(CooperativeMatrixMemoryAccess::MemoryAccessTemporalMask);

  Value *threadId = getLaneNumber(builder);
  Value *colOffsetPerLane = builder.CreateSRem(threadId, builder.getInt32(16));
  Value *offset = builder.CreateMul(colOffsetPerLane, stride);

  Value *elemPtr = builder.CreateGEP(elemTy, dataPtr, offset);
  Value *elemVal = builder.CreateStore(data, elemPtr, isVolatile);
  if (isCoherent && !(addrSpace == ADDR_SPACE_LOCAL && dataBitwidth < 32))
    cast<LoadInst>(elemVal)->setAtomic(AtomicOrdering::Unordered);
  if (isTemporal)
    cast<LoadInst>(elemVal)->setMetadata(LLVMContext::MD_nontemporal, MDNode::get(builder.getContext(), {}));

  m_coopRowAccCalls.push_back(&store);
}

// =====================================================================================================================
// Visit "CooperativeRowAccAccumulateModeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeRowAccAccumulateModeOp(CooperativeRowAccAccumulateModeOp &accumulateMode) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&accumulateMode);

  Value *rowAccVal = accumulateMode.getRowAcc();
  auto elemType = accumulateMode.getElemType();

  assert(builder.transCooperativeMatrixElementType(elemType) == accumulateMode.getType());
  assert(accumulateMode.getType() == rowAccVal->getType());

  if (m_gfxIp.major >= 12)
    rowAccVal = cooperativeRowAccConvertToAccumulateMode(builder, getLaneNumber(builder), rowAccVal, elemType);

  accumulateMode.replaceAllUsesWith(rowAccVal);
  m_coopRowAccCalls.push_back(&accumulateMode);
}

// =====================================================================================================================
// Visit "CooperativeRowAccFinalizeModeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeRowAccFinalizeModeOp(CooperativeRowAccFinalizeModeOp &finalize) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&finalize);

  Value *rowAccVal = finalize.getRowAcc();
  auto elemType = finalize.getElemType();

  assert(builder.transCooperativeMatrixElementType(elemType) == finalize.getType());
  assert(finalize.getType() == rowAccVal->getType());

  if (m_gfxIp.major >= 12)
    rowAccVal = cooperativeRowAccConvertToFinalizeMode(builder, rowAccVal, elemType);

  finalize.replaceAllUsesWith(rowAccVal);
  m_coopRowAccCalls.push_back(&finalize);
}

// =====================================================================================================================
// Visit "CooperativeRowAccSplatOp" instruction
//
// @param inst : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeRowAccSplatOp(CooperativeRowAccSplatOp &splat) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&splat);

  Value *scalar = splat.getScalar();

  assert(builder.transCooperativeMatrixElementType(splat.getElemType()) == scalar->getType());

  splat.replaceAllUsesWith(scalar);
  m_coopRowAccCalls.push_back(&splat);
}

// =====================================================================================================================
// Visit "CooperativeRowAccExpandOp" instruction
//
// @param inst : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeRowAccExpandOp(CooperativeRowAccExpandOp &expand) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&expand);

  auto rowAccVal = expand.getRowAcc();
  auto rowAccElemType = expand.getRowAccElemType();
  auto matrixElemType = expand.getMatrixElemType();
  auto matrixLayout = expand.getMatrixLayout();
  auto colMajor = expand.getColMajor();

  assert(builder.getCooperativeMatrixTy(matrixElemType, matrixLayout) == expand.getType());
  assert(rowAccElemType == CooperativeMatrixElementType::Float16 ||
         rowAccElemType == CooperativeMatrixElementType::Float32 ||
         rowAccElemType == CooperativeMatrixElementType::Int32);
  assert(matrixElemType == CooperativeMatrixElementType::Float16 ||
         matrixElemType == CooperativeMatrixElementType::Float32 ||
         matrixElemType == CooperativeMatrixElementType::Int32);

  // Element type convert.
  if (rowAccElemType == CooperativeMatrixElementType::Float16 &&
      matrixElemType == CooperativeMatrixElementType::Float32)
    rowAccVal = builder.CreateFPExt(rowAccVal, builder.getFloatTy());
  else if (rowAccElemType == CooperativeMatrixElementType::Float32 &&
           matrixElemType == CooperativeMatrixElementType::Float16)
    rowAccVal = builder.CreateFPTrunc(rowAccVal, builder.getHalfTy());
  else
    assert(rowAccElemType == matrixElemType);

  assert(matrixLayout == CooperativeMatrixLayout::AccumulatorMatrixLayout);
  auto props = getTypeProperties(matrixElemType, matrixLayout);
  Type *flatType =
      FixedVectorType::get(builder.transCooperativeMatrixElementType(matrixElemType), props.numFlatElements);
  Value *flatVec = PoisonValue::get(flatType);

  if (!colMajor) {
    for (unsigned idx = 0; idx < props.numFlatElements; idx++)
      flatVec = builder.CreateInsertElement(flatVec, rowAccVal, idx);
  } else {
    auto mapFuncDpp = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                         ArrayRef<Value *> passthroughArgs) -> Value * {
      return builder.CreateIntrinsic(
          Intrinsic::amdgcn_mov_dpp, builder.getInt32Ty(),
          {mappedArgs[0], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
    };

    auto waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
    assert(waveSize == 32 || waveSize == 64);

    DppCtrl shuffleCtrl[4] = {DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX)};
    DppCtrl expandCtrl[8] = {DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX),
                             DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX), DppCtrl(UINT32_MAX)};

    if (matrixLayout == CooperativeMatrixLayout::AccumulatorMatrixLayout) {
      if (waveSize == 64) {
        // Gfx11 AccumulatorMatrixLayout F32/I32@Wave64:
        // VGPR/Lane . 0 . . . . 1 . . . . 15 . . . . 16 . . . . 31
        // VGPR[8]:  C0_0 . . . C0_1 . . . C0_f . . . C1_0 . . . C1_f
        // VGPR[9]:  C4_0 . . . C4_1 . . . C4_f . . . C5_0 . . . C5_f
        // VGPR[10]: C8_0 . . . C8_1 . . . C8_f . . . C9_0 . . . C9_f
        // VGPR[11]: Cc_0 . . . Cc_1 . . . Cc_f . . . Cd_0 . . . Cd_f
        // VGPR/Lane . 32 . . . 33 . . . . 47 . . . . 48 . . . . 63
        // VGPR[8]:  C2_0 . . . C2_1 . . . C2_f . . . C3_0 . . . C3_f
        // VGPR[9]:  C6_0 . . . C6_1 . . . C6_f . . . C7_0 . . . C7_f
        // VGPR[10]: Ca_0 . . . Ca_1 . . . Ca_f . . . Cb_0 . . . Cb_f
        // VGPR[11]: Ce_0 . . . Ce_1 . . . Ce_f . . . Cf_0 . . . Cf_f
        // Row accumulator data is in finalized state and duplciated in each 16 lanes.
        // Change row accumulator data lanes:
        // 16 - 31 to [C1, C2, C3, C4, C5, C6, C7, C8, C9, Ca, Cb, Cc, Cd, Ce, Cf, XX].
        // 32 - 47 to [C2, C3, C4, C5, C6, C7, C8, C9, Ca, Cb, Cc, Cd, Ce, Cf, XX, XX].
        // 48 - 63 to [C3, C4, C5, C6, C7, C8, C9, Ca, Cb, Cc, Cd, Ce, Cf, XX, XX, XX].
        shuffleCtrl[1] = DppCtrl::DppRowSl1;
        shuffleCtrl[2] = DppCtrl::DppRowSl2;
        shuffleCtrl[3] = DppCtrl::DppRowSl3;
        expandCtrl[0] = DppCtrl::DppRowShare0;
        expandCtrl[1] = DppCtrl::DppRowShare4;
        expandCtrl[2] = DppCtrl::DppRowShare8;
        expandCtrl[3] = DppCtrl::DppRowShare12;
      } else {
        // Gfx11 AccumulatorMatrixLayout F32/I32@Wave32:
        // VGPR/Lane . 0 . . . . 1 . . . . 15 . . . . 16 . . . . 31
        // VGPR[8]:  C0_0 . . . C0_1 . . . C0_f . . . C1_0 . . . C1_f
        // VGPR[9]:  C2_0 . . . C2_1 . . . C2_f . . . C3_0 . . . C3_f
        // VGPR[10]: C4_0 . . . C4_1 . . . C4_f . . . C5_0 . . . C5_f
        // VGPR[11]: C6_0 . . . C6_1 . . . C6_f . . . C7_0 . . . C7_f
        // VGPR[12]: C8_0 . . . C8_1 . . . C8_f . . . C9_0 . . . C9_f
        // VGPR[13]: Ca_0 . . . Ca_1 . . . Ca_f . . . Cb_0 . . . Cb_f
        // VGPR[14]: Cc_0 . . . Cc_1 . . . Cc_f . . . Cd_0 . . . Cd_f
        // VGPR[15]: Ce_0 . . . Ce_1 . . . Ce_f . . . Cf_0 . . . Cf_f
        // Row accumulator data is in finalized state and duplciated in each 16 lanes.
        // Change row accumulator data lanes:
        // 16 - 31 to [C1, C2, C3, C4, C5, C6, C7, C8, C9, Ca, Cb, Cc, Cd, Ce, Cf, XX].
        shuffleCtrl[1] = DppCtrl::DppRowSl1;
        constexpr DppCtrl ctrl[] = {DppCtrl::DppRowShare0,  DppCtrl::DppRowShare2, DppCtrl::DppRowShare4,
                                    DppCtrl::DppRowShare6,  DppCtrl::DppRowShare8, DppCtrl::DppRowShare10,
                                    DppCtrl::DppRowShare12, DppCtrl::DppRowShare14};
        memcpy(expandCtrl, ctrl, sizeof(ctrl));
      }
    } else
      llvm_unreachable("unknow layout");

    Value *rowAccShuffleVal = rowAccVal;
    // Shuffle the data in each group of row accumulator data. wave64 have 4 x group16.
    for (unsigned idx = 0; idx < 4; idx++) {
      if (shuffleCtrl[idx] != DppCtrl(UINT32_MAX)) {
        rowAccShuffleVal =
            builder.CreateMapToSimpleType(mapFuncDpp, rowAccShuffleVal,
                                          {builder.getInt32((unsigned)(shuffleCtrl[idx])), builder.getInt32(1 << idx),
                                           builder.getInt32(0xF), builder.getInt1(true)});
      }
    }

    for (unsigned idx = 0; idx < props.numFlatElements; idx++) {
      assert(expandCtrl[idx] != DppCtrl(UINT32_MAX));
      Value *outputVal =
          builder.CreateMapToSimpleType(mapFuncDpp, rowAccShuffleVal,
                                        {builder.getInt32((unsigned)(expandCtrl[idx])), builder.getInt32(0xF),
                                         builder.getInt32(0xF), builder.getInt1(true)});
      flatVec = builder.CreateInsertElement(flatVec, outputVal, idx);
    }
  }
  Value *resultVal = convFlatVecToCoopMatrixVec(builder, flatVec, matrixElemType, matrixLayout);

  expand.replaceAllUsesWith(resultVal);
  m_coopRowAccCalls.push_back(&expand);
}

// =====================================================================================================================
// Visit "CooperativeRowAccSumAccumulateOp" instruction
//
// @param inst : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeRowAccSumAccumulateOp(CooperativeRowAccSumAccumulateOp &sumAccumulate) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&sumAccumulate);

  auto matrixVal = sumAccumulate.getMatrix();
  auto matrixElemType = sumAccumulate.getMatrixElemType();
  auto matrixLayout = sumAccumulate.getMatrixLayout();
  Value *rowAccVal = sumAccumulate.getRowAcc();
  auto rowAccElemType = sumAccumulate.getRowAccElemType();
  auto isSigned = sumAccumulate.getIsSigned();

  assert(matrixLayout == CooperativeMatrixLayout::FactorMatrixLayout);

  Value *vcFlat = convCoopMatrixVecToFlatVec(builder, matrixVal, matrixElemType, matrixLayout);
  const unsigned numElems = cast<FixedVectorType>(vcFlat->getType())->getNumElements();

  Value *sumVal = rowAccVal;
  if (matrixElemType == CooperativeMatrixElementType::Float16) {
    assert(numElems % 2 == 0);

    // Use fdot2 for f32 accumulate.
    if (rowAccElemType == CooperativeMatrixElementType::Float32) {
      Value *constOne = builder.getFpConstant(builder.getHalfTy(), APFloat(1.0));
      Value *constVector = PoisonValue::get(FixedVectorType::get(builder.getHalfTy(), 2));
      constVector = builder.CreateInsertElement(constVector, constOne, uint64_t(0));
      constVector = builder.CreateInsertElement(constVector, constOne, 1);

      for (unsigned i = 0; i < numElems / 2; i++) {
        Value *vector = builder.CreateShuffleVector(vcFlat, ArrayRef<int>{int(i * 2), int(i * 2 + 1)});
        sumVal =
            builder.CreateIntrinsic(Intrinsic::amdgcn_fdot2, {}, {vector, constVector, sumVal, builder.getFalse()});
      }
    } else {
      assert(rowAccElemType == CooperativeMatrixElementType::Float16);
      for (unsigned i = 0; i < numElems; i++) {
        auto val = builder.CreateExtractElement(vcFlat, i);
        sumVal = builder.CreateFAdd(val, sumVal);
      }
    }
  } else if (matrixElemType == CooperativeMatrixElementType::Int16) {
    assert(rowAccElemType == CooperativeMatrixElementType::Int32);
    assert(numElems % 2 == 0);

    // No dot2 for int16 on gfx11
    if (m_gfxIp.major >= 11) {
      for (unsigned i = 0; i < numElems; i++) {
        auto val = builder.CreateExtractElement(vcFlat, i);
        if (isSigned)
          val = builder.CreateSExt(val, builder.getInt32Ty());
        else
          val = builder.CreateZExt(val, builder.getInt32Ty());
        sumVal = builder.CreateAdd(val, sumVal);
      }
    } else {
      Value *constVector = PoisonValue::get(FixedVectorType::get(builder.getInt16Ty(), 2));
      constVector = builder.CreateInsertElement(constVector, builder.getInt16(1), uint64_t(0));
      constVector = builder.CreateInsertElement(constVector, builder.getInt16(1), 1);

      for (unsigned i = 0; i < numElems / 2; i++) {
        Value *vector = builder.CreateShuffleVector(vcFlat, ArrayRef<int>{int(i * 2), int(i * 2 + 1)});
        sumVal = builder.CreateIntrinsic(isSigned ? Intrinsic::amdgcn_sdot2 : Intrinsic::amdgcn_udot2, {},
                                         {vector, constVector, sumVal, builder.getFalse()});
      }
    }
  } else if (matrixElemType == CooperativeMatrixElementType::Int8) {
    assert(rowAccElemType == CooperativeMatrixElementType::Int32);
    assert(numElems % 4 == 0);

    auto packedType = FixedVectorType::get(builder.getInt32Ty(), numElems / 4);
    Value *vcPacked = builder.CreateBitCast(vcFlat, packedType);

    auto constPackedOne = builder.getInt32(0x01010101);
    // Using dot4 intrinsic for accumulate.
    for (unsigned i = 0; i < packedType->getNumElements(); i++) {
      auto packedVal = builder.CreateExtractElement(vcPacked, i);
      if (m_gfxIp.major >= 11) {
        // Use sudot4 for gfx11+
        sumVal = builder.CreateIntrinsic(Intrinsic::amdgcn_sudot4, {},
                                         {builder.getInt1(isSigned), packedVal, builder.getInt1(isSigned),
                                          constPackedOne, sumVal, builder.getFalse()});
      } else {
        // Use sdot4 and udot4 for gfx10
        sumVal = builder.CreateIntrinsic(isSigned ? Intrinsic::amdgcn_sdot4 : Intrinsic::amdgcn_udot4, {},
                                         {packedVal, constPackedOne, sumVal, builder.getFalse()});
      }
    }
  } else
    llvm_unreachable("not supported element type for CooperativeRowAccSumAccumulate");

  sumAccumulate.replaceAllUsesWith(sumVal);
  m_coopRowAccCalls.push_back(&sumAccumulate);
}

// =====================================================================================================================
// Visit "CooperativeRowAccScalarOp" instruction
//
// @param inst : The dialect instruction to process
void LowerCooperativeMatrix::visitCooperativeRowAccScalarOp(CooperativeRowAccScalarOp &scalar) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&scalar);

  auto elemType = scalar.getElemType();
  Value *rowAccVal = scalar.getRowAcc();
  Value *scalarVal = scalar.getScalar();
  auto coopMatArithOp = scalar.getBinop();
  bool accumulateMode = scalar.getAccumulateMode();

  assert(builder.transCooperativeMatrixElementType(elemType) == rowAccVal->getType());
  assert(builder.transCooperativeMatrixElementType(elemType) == scalarVal->getType());

  bool needHandleAccumulateMode = accumulateMode && (m_gfxIp.major >= 12);

  if (needHandleAccumulateMode) {
    if (coopMatArithOp == CooperativeMatrixArithOp::FDiv || coopMatArithOp == CooperativeMatrixArithOp::IMul ||
        coopMatArithOp == CooperativeMatrixArithOp::FMul) {
      // Assume above operation have same result in accumulate mode as ScalarOp(A + B, Scalar) = ScalarOp(A, Scalar) +
      // ScalarOp(B, Scalar)
      needHandleAccumulateMode = false;
    } else if (coopMatArithOp == CooperativeMatrixArithOp::IAdd || coopMatArithOp == CooperativeMatrixArithOp::FAdd ||
               coopMatArithOp == CooperativeMatrixArithOp::ISub || coopMatArithOp == CooperativeMatrixArithOp::FSub) {
      // Assume above operation have same result in accumulate mode as ScalarOp(A + B, Scalar) = ScalarOp(A, Scalar) +
      // ScalarOp(B, 0)
      // Make scalar value only valid part lanes as accumulate mode.
      scalarVal = cooperativeRowAccConvertToAccumulateMode(builder, getLaneNumber(builder), scalarVal, elemType);
      needHandleAccumulateMode = false;
    }
  }

  if (needHandleAccumulateMode)
    scalarVal = cooperativeRowAccConvertToFinalizeMode(builder, scalarVal, elemType);

  Value *resultVal = nullptr;
  switch (coopMatArithOp) {
  case CooperativeMatrixArithOp::IAdd:
    resultVal = builder.CreateAdd(rowAccVal, scalarVal);
    break;
  case CooperativeMatrixArithOp::FAdd:
    resultVal = builder.CreateFAdd(rowAccVal, scalarVal);
    break;
  case CooperativeMatrixArithOp::ISub:
    resultVal = builder.CreateSub(rowAccVal, scalarVal);
    break;
  case CooperativeMatrixArithOp::FSub:
    resultVal = builder.CreateFSub(rowAccVal, scalarVal);
    break;
  case CooperativeMatrixArithOp::IMul:
    resultVal = builder.CreateMul(rowAccVal, scalarVal);
    break;
  case CooperativeMatrixArithOp::FMul:
    resultVal = builder.CreateFMul(rowAccVal, scalarVal);
    break;
  case CooperativeMatrixArithOp::FDiv:
    resultVal = builder.CreateFDiv(rowAccVal, scalarVal);
    break;
  case CooperativeMatrixArithOp::SDiv:
    resultVal = builder.CreateSDiv(rowAccVal, scalarVal);
    break;
  case CooperativeMatrixArithOp::UDiv:
    resultVal = builder.CreateUDiv(rowAccVal, scalarVal);
    break;
  default:
    llvm_unreachable("unsupported binary operation for cooperative row acc!");
  }

  if (needHandleAccumulateMode)
    resultVal = cooperativeRowAccConvertToAccumulateMode(builder, getLaneNumber(builder), resultVal, elemType);

  scalar.replaceAllUsesWith(resultVal);
  m_coopRowAccCalls.push_back(&scalar);
}

// =====================================================================================================================
// Convert row acc to finalize mode by adding the interleave 16 lanes.
//
// @param builder : The IR builder to create and insert IR instruction
// @param rowAccVal : The cooperative rowAcc value
// @param elemType : The component type of the rowAcc value
Value *LowerCooperativeMatrix::cooperativeRowAccConvertToFinalizeMode(BuilderBase &builder, llvm::Value *rowAccVal,
                                                                      CooperativeMatrixElementType elemType) {
  unsigned LaneSelBits[2] = {0x76543210, 0xfedcba98};
  auto mapFuncX16 = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                       ArrayRef<Value *> passthroughArgs) -> Value * {
    Type *const int32Ty = builder.getInt32Ty();

    return builder.CreateIntrinsic(
        int32Ty, Intrinsic::amdgcn_permlanex16,
        {mappedArgs[0], mappedArgs[1], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };

  Value *swapped = builder.CreateMapToSimpleType(
      mapFuncX16,
      {
          rowAccVal,
          rowAccVal,
      },
      {builder.getInt32(LaneSelBits[0]), builder.getInt32(LaneSelBits[1]), builder.getFalse(), builder.getFalse()});

  switch (elemType) {
  case CooperativeMatrixElementType::Float32:
  case CooperativeMatrixElementType::Float16:
    rowAccVal = builder.CreateFAdd(rowAccVal, swapped);
    break;
  case CooperativeMatrixElementType::Int32:
    rowAccVal = builder.CreateAdd(rowAccVal, swapped);
    break;
  case CooperativeMatrixElementType::Int8:
  case CooperativeMatrixElementType::Int16:
  case CooperativeMatrixElementType::Float16Packed:
    llvm_unreachable("not supported element type for row acc");
  default:
    llvm_unreachable("unknown element type");
  }

  return rowAccVal;
}

// =====================================================================================================================
// Convert row acc to accumulate mode by force set zero on the duplicated lanes in each 32 waves.
//
// @param builder : The IR builder to create and insert IR instruction
// @param rowAccVal : The cooperative rowAcc value
// @param threadId : The current lane index
// @param elemType : The component type of the rowAcc value
Value *LowerCooperativeMatrix::cooperativeRowAccConvertToAccumulateMode(BuilderBase &builder, llvm::Value *rowAccVal,
                                                                        llvm::Value *threadId,
                                                                        CooperativeMatrixElementType elemType) {
  Value *zero = nullptr;
  switch (elemType) {
  case CooperativeMatrixElementType::Float32:
    zero = builder.getFpConstant(builder.getFloatTy(), APFloat(0.0));
    break;
  case CooperativeMatrixElementType::Float16:
    zero = builder.getFpConstant(builder.getHalfTy(), APFloat(0.0));
    break;
  case CooperativeMatrixElementType::Int32:
    zero = builder.getInt32(0);
    break;
  case CooperativeMatrixElementType::Int8:
  case CooperativeMatrixElementType::Int16:
  case CooperativeMatrixElementType::Float16Packed:
    llvm_unreachable("not supported element type for cooperative row acc");
  default:
    llvm_unreachable("unknown element type");
  }

  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));
  return builder.CreateSelect(isEvenGroup, rowAccVal, zero);
}

// =====================================================================================================================
// Process all the cooperative row acc operations on module
//
// @param [in/out] module :  LLVM module to be run on
void LowerCooperativeMatrix::processCoopRowAccFunction(Module &module) {
  static auto visitor = llvm_dialects::VisitorBuilder<LowerCooperativeMatrix>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add(&LowerCooperativeMatrix::visitCooperativeRowAccLoadOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeRowAccStoreOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeRowAccAccumulateModeOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeRowAccFinalizeModeOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeRowAccSplatOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeRowAccExpandOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeRowAccSumAccumulateOp)
                            .add(&LowerCooperativeMatrix::visitCooperativeRowAccScalarOp)
                            .build();

  visitor.visit(*this, module);

  for (auto callInst : m_coopRowAccCalls) {
    callInst->dropAllReferences();
    callInst->eraseFromParent();
  }
  m_coopRowAccCalls.clear();
}

} // namespace lgc
