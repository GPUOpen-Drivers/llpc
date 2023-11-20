/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
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
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
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

  if (runImpl(module, pipelineShaders, pipelineState)) {
    PreservedAnalyses PA;
    PA.preserveSet<CFGAnalyses>();
    return PA;
  }
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Run the on a module
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool LowerCooperativeMatrix::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                     PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Cooperative-Matrix\n");
  Patch::init(&module);
  m_pipelineState = pipelineState;
  m_pipelineShaders = &pipelineShaders;
  m_shaderStage = ShaderStageCompute;
  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  SmallVector<Function *, 16> lowerCoopMatrixCallees;
  for (auto &func : module) {
    auto name = func.getName();
    if (name.startswith(lgcName::CooperativeMatrix))
      lowerCoopMatrixCallees.push_back(&func);
  }
  if (lowerCoopMatrixCallees.empty())
    return false;

  processCoopMatrixFunction(lowerCoopMatrixCallees);

  for (auto callInst : m_coopMatrixCalls) {
    callInst->dropAllReferences();
    callInst->eraseFromParent();
  }
  m_coopMatrixCalls.clear();
  return true;
}

// =====================================================================================================================
// Run the on a module
//
// @param coopMatrixCallees : Function array for the cooperativeMatrix
void LowerCooperativeMatrix::processCoopMatrixFunction(ArrayRef<Function *> coopMatrixCallees) {
  for (auto callee : coopMatrixCallees) {
    for (auto user : callee->users()) {
      if (CallInst *callInst = dyn_cast<CallInst>(user)) {
        visitCallInst(*callInst);
      }
    }
  }
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void LowerCooperativeMatrix::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  m_coopMatrixCalls.push_back(&callInst);

  BuilderCommon builder(*m_context);
  builder.SetInsertPoint(&callInst);

  auto mangledName = callee->getName();
  if (mangledName.startswith(lgcName::CooperativeMatrixLength)) {
    auto layout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(1))->getZExtValue());
    callInst.replaceAllUsesWith(builder.getInt32(getLength(layout)));
  } else if (mangledName.startswith(lgcName::CooperativeMatrixExtract)) {
    Value *matrix = callInst.getOperand(0);
    Value *index = callInst.getOperand(1);
    auto elemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(2))->getZExtValue());
    auto layout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(3))->getZExtValue());
    Value *result = cooperativeMatrixExtract(builder, matrix, index, elemType, layout);
    result->takeName(&callInst);
    callInst.replaceAllUsesWith(result);
  } else if (mangledName.startswith(lgcName::CooperativeMatrixInsert)) {
    Value *matrix = callInst.getOperand(0);
    Value *value = callInst.getOperand(1);
    Value *index = callInst.getOperand(2);
    auto elemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(3))->getZExtValue());
    auto layout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(4))->getZExtValue());
    Value *result = cooperativeMatrixInsert(builder, matrix, value, index, elemType, layout);
    result->takeName(&callInst);
    callInst.replaceAllUsesWith(result);
  } else if (mangledName.startswith(lgcName::CooperativeMatrixLoad)) {
    Value *dataPtr = callInst.getOperand(0);
    Value *stride = callInst.getOperand(1);
    bool colMajor = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
    Builder::CooperativeMatrixElementType elemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(3))->getZExtValue());
    Builder::CooperativeMatrixLayout layout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(4))->getZExtValue());
    unsigned memoryAccess = cast<ConstantInt>(callInst.getOperand(5))->getZExtValue();

    Value *loadVal = cooperativeMatrixLoadInternal(dataPtr, stride, colMajor, elemType, layout, memoryAccess,
                                                   callInst.getName(), &callInst);
    callInst.replaceAllUsesWith(loadVal);

  } else if (mangledName.startswith(lgcName::CooperativeMatrixStore)) {
    Value *dataPtr = callInst.getOperand(0);
    Value *stride = callInst.getOperand(1);
    bool colMajor = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
    Builder::CooperativeMatrixElementType elemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(3))->getZExtValue());
    Builder::CooperativeMatrixLayout layout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(4))->getZExtValue());
    unsigned memoryAccess = cast<ConstantInt>(callInst.getOperand(5))->getZExtValue();
    Value *vecVal = callInst.getOperand(6);

    cooperativeMatrixStoreInternal(dataPtr, stride, colMajor, elemType, layout, memoryAccess, vecVal,
                                   callInst.getName(), &callInst);

  } else if (mangledName.startswith(lgcName::CooperativeMatrixConvert)) {
    CastInst::CastOps castOp =
        static_cast<CastInst::CastOps>(cast<ConstantInt>(callInst.getOperand(0))->getZExtValue());
    Value *source = callInst.getOperand(1);
    Builder::CooperativeMatrixElementType srcElemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(2))->getZExtValue());
    Builder::CooperativeMatrixElementType dstElemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(3))->getZExtValue());
    Builder::CooperativeMatrixLayout srcLayout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(4))->getZExtValue());
    Builder::CooperativeMatrixLayout dstLayout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(5))->getZExtValue());
    Value *resultVal = cooperativeMatrixConvert(castOp, source, srcElemType, dstElemType, srcLayout, dstLayout,
                                                callInst.getName(), &callInst);
    if ((cast<FixedVectorType>(resultVal->getType())->getNumElements() == 4) &&
        (dstLayout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout ||
         dstLayout == Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout ||
         dstLayout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout)) {
      // for wave64 needs shuffleVector from V4 to V8 as frontend will always recognize V8 not care wave32 or wave64
      resultVal = builder.CreateShuffleVector(resultVal, PoisonValue::get(resultVal->getType()),
                                              ArrayRef<int>{0, 1, 2, 3, 4, 5, 6, 7});
    }
    callInst.replaceAllUsesWith(resultVal);

  } else if (mangledName.startswith(lgcName::CooperativeMatrixTranspose)) {
    Value *matrix = callInst.getOperand(0);
    Builder::CooperativeMatrixElementType elemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(1))->getZExtValue());
    Builder::CooperativeMatrixLayout srcLayout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(2))->getZExtValue());

    Value *resultVal = cooperativeMatrixTranspose(matrix, elemType, srcLayout, callInst.getName(), &callInst);
    callInst.replaceAllUsesWith(resultVal);

  } else if (mangledName.startswith(lgcName::CooperativeMatrixBinOp)) {
    Builder::CooperativeMatrixArithOp coopMatArithOp =
        static_cast<Builder::CooperativeMatrixArithOp>(cast<ConstantInt>(callInst.getOperand(0))->getZExtValue());
    Value *lhs = callInst.getOperand(1);
    Value *rhs = callInst.getOperand(2);
    Builder::CooperativeMatrixElementType elemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(3))->getZExtValue());
    Builder::CooperativeMatrixLayout srcLayout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(4))->getZExtValue());

    Value *resultVal =
        cooperativeMatrixBinaryOp(coopMatArithOp, lhs, rhs, elemType, srcLayout, callInst.getName(), &callInst);
    callInst.replaceAllUsesWith(resultVal);

  } else if (mangledName.startswith(lgcName::CooperativeMatrixTimesScalar)) {
    Value *matrix = callInst.getOperand(0);
    Value *scalar = callInst.getOperand(1);
    Builder::CooperativeMatrixElementType elemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(2))->getZExtValue());
    Builder::CooperativeMatrixLayout srcLayout =
        static_cast<Builder::CooperativeMatrixLayout>(cast<ConstantInt>(callInst.getOperand(3))->getZExtValue());

    Value *resultVal = coopMatrixTimesScalar(matrix, scalar, elemType, srcLayout, callInst.getName(), &callInst);
    callInst.replaceAllUsesWith(resultVal);

  } else if (mangledName.startswith(lgcName::CooperativeMatrixMulAdd)) {
    Value *matrixA = callInst.getOperand(0);
    Value *matrixB = callInst.getOperand(1);
    Value *matrixC = callInst.getOperand(2);
    bool isSignedA = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue();
    bool isSignedB = cast<ConstantInt>(callInst.getOperand(4))->getZExtValue();
    bool isSat = cast<ConstantInt>(callInst.getOperand(5))->getZExtValue();
    Builder::CooperativeMatrixElementType accumElemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(6))->getZExtValue());
    Builder::CooperativeMatrixElementType factorElemType =
        static_cast<Builder::CooperativeMatrixElementType>(cast<ConstantInt>(callInst.getOperand(7))->getZExtValue());
    Value *resultVal = cooperativeMatrixMulAdd(matrixA, matrixB, matrixC, isSignedA, isSignedB, isSat, accumElemType,
                                               factorElemType, callInst.getName(), &callInst);
    callInst.replaceAllUsesWith(resultVal);

  } else {
    llvm_unreachable("Should never be called!");
  }
}

// =====================================================================================================================
// Get the "length" of a matrix of the given layout, i.e. the number of matrix components stored per lane.
//
// @param layout : the matrix layout
unsigned LowerCooperativeMatrix::getLength(Builder::CooperativeMatrixLayout layout) const {
  auto waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  switch (layout) {
  case BuilderCommon::FactorMatrixLayout:
    return 16;
  case BuilderCommon::AccumulatorMatrixLayout: {
    return waveSize == 32 ? 8 : 4;
  }
  case BuilderCommon::Gfx10AccumulatorMatrixLayout:
  case BuilderCommon::Gfx10Accumulator16bitMatrixLayout:
    return 8;
  default:
    llvm_unreachable("unhandled matrix layout");
  }
}

// =====================================================================================================================
// Determine properties of the cooperative matrix type depending on element type, layout, and wave size.
//
// @param elemType : the matrix element type
// @param layout : the matrix layout
// @returns : the type properties
LowerCooperativeMatrix::TypeProperties
LowerCooperativeMatrix::getTypeProperties(Builder::CooperativeMatrixElementType elemType,
                                          Builder::CooperativeMatrixLayout layout) const {
  TypeProperties props;

  props.matrixElementStride = 1;

  switch (elemType) {
  case Builder::CooperativeMatrixElementType::Float32:
  case Builder::CooperativeMatrixElementType::Int32:
    props.numMatrixElements = 8;
    props.numMatrixWords = 8;
    break;
  case Builder::CooperativeMatrixElementType::Float16:
  case Builder::CooperativeMatrixElementType::Int16:
    props.numMatrixElements = 16;
    props.numMatrixWords = 8;
    break;
  case Builder::CooperativeMatrixElementType::Int8:
    props.numMatrixElements = 16;
    props.numMatrixWords = 4;
    break;
  default:
    llvm_unreachable("unknown element type");
  }

  auto waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  if (layout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) {
    assert(elemType != Builder::CooperativeMatrixElementType::Float32 &&
           elemType != Builder::CooperativeMatrixElementType::Int32);
    props.numFlatElements = 16;
  } else if (layout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout) {
    props.numFlatElements = waveSize == 32 ? 8 : 4;
    if (elemType == Builder::CooperativeMatrixElementType::Float16 ||
        elemType == Builder::CooperativeMatrixElementType::Int16) {
      props.matrixElementStride = 2;
    }
  } else if (layout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout ||
             layout == Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) {
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
                                                          Builder::CooperativeMatrixElementType elemType,
                                                          Builder::CooperativeMatrixLayout layout) {
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
                                                          Builder::CooperativeMatrixElementType elemType,
                                                          Builder::CooperativeMatrixLayout layout) {
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
LowerCooperativeMatrix::computeAddressing(Builder::CooperativeMatrixLayout layout,
                                          Builder::CooperativeMatrixElementType elemType, int waveSize, Value *stride,
                                          bool isColMajor, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *threadId = getLaneNumber(builder);
  ComputeAddressInfo addrInfo;
  Value *rowOffsetInFirstVgpr = nullptr;
  Value *colOffsetPerLane = builder.CreateSRem(threadId, builder.getInt32(16));
  addrInfo.microStep = builder.getInt32(0);
  addrInfo.microCount = 1;
  (void)elemType;

  if (layout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) {
    rowOffsetInFirstVgpr = builder.getInt32(0);
    addrInfo.macroStep = builder.getInt32(1);
  } else if (layout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout) {
    rowOffsetInFirstVgpr = builder.CreateUDiv(threadId, builder.getInt32(16));
    addrInfo.macroStep = (waveSize == 64 ? builder.getInt32(4) : builder.getInt32(2));
  } else if (layout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout) {
    rowOffsetInFirstVgpr = builder.CreateUDiv(builder.CreateSRem(threadId, builder.getInt32(32)), builder.getInt32(16));
    addrInfo.macroStep = builder.getInt32(2);
  } else if (layout == Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) {
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
// Load contiguous elements from the specified location of the memory.
// @param dataPtr : The pointer to a data array.
// @param stride : The stride in bytes in memory between the first elements of consecutive rows (orcolumns) in the
// source data. Guaranteed to be a multiple of the matrix element size.
// @param isColMajor : Identify the order for the data stored in memory, col-major/row-major
// @param elemType : The element type for the matrix
// @param layout : This is identify for factor(A/B) or accumulator(C) for 16 bit element matrix.
// @param memoryAccess : The memory operands which provide:isVolatile/isTemporal/isCoherent
// additional operands, maybe volatile/Aligned/Nontemporal/MakePointerAvailable
// /MakePointerVisible/NonPrivatePointer usded by CooperativeMatrix Load/Store.
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixLoadInternal(Value *dataPtr, Value *stride, bool isColMajor,
                                                             Builder::CooperativeMatrixElementType elemType,
                                                             Builder::CooperativeMatrixLayout layout,
                                                             unsigned memoryAccess, const Twine &instName,
                                                             Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  auto waveSize = m_pipelineState->getShaderWaveSize(getShaderStage(builder.GetInsertBlock()->getParent()));
  assert(waveSize == 32 || waveSize == 64);

  // Calc element offset in memory
  Type *elemTy = builder.transCooperativeMatrixElementType(elemType);
  const unsigned dataBitwidth = elemTy->getScalarSizeInBits();
  const unsigned addrSpace = dataPtr->getType()->getPointerAddressSpace();
  assert(addrSpace == ADDR_SPACE_LOCAL || addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER || addrSpace == ADDR_SPACE_GLOBAL);

  stride = builder.CreateExactSDiv(stride, builder.getInt32(dataBitwidth / 8));

  // calc memoryAccess
  bool isVolatile = memoryAccess & Builder::MemoryAccessVolatileMask;
  bool isCoherent = memoryAccess & Builder::MemoryAccessCoherentMask;
  bool isTemporal = memoryAccess & Builder::MemoryAccessTemporalMask;

  auto props = getTypeProperties(elemType, layout);
  auto addrInfo = computeAddressing(layout, elemType, waveSize, stride, isColMajor, insertPos);

  Value *vecVal = PoisonValue::get(FixedVectorType::get(elemTy, props.numFlatElements));
  for (unsigned idx = 0; idx < props.numFlatElements; ++idx) {
    Value *offset = builder.CreateAdd(
        addrInfo.base, builder.CreateMul(addrInfo.macroStep, builder.getInt32(idx / addrInfo.microCount)));
    offset =
        builder.CreateAdd(offset, builder.CreateMul(addrInfo.microStep, builder.getInt32(idx % addrInfo.microCount)));

    Value *elePtr = builder.CreateGEP(elemTy, dataPtr, offset);
    Value *eleVal = builder.CreateLoad(elemTy, elePtr, isVolatile, instName);
    if (isCoherent && !(addrSpace == ADDR_SPACE_LOCAL && dataBitwidth < 32))
      cast<LoadInst>(eleVal)->setAtomic(AtomicOrdering::Unordered);
    if (isTemporal)
      cast<LoadInst>(eleVal)->setMetadata(LLVMContext::MD_nontemporal, MDNode::get(builder.getContext(), {}));
    vecVal = builder.CreateInsertElement(vecVal, eleVal, idx);
  }

  Value *coMatrix = convFlatVecToCoopMatrixVec(builder, vecVal, elemType, layout);
  return coMatrix;
}

// =====================================================================================================================
// Store a contiguous elements from the specified location of the memory.
//
// @param dataPtr : The pointer to a data array.
// @param stride : The stride in bytes between the first elements of consecutive rows (or columns) in the destination.
// Guaranteed to be a multiple of the element size.
// @param colMajor : Identify the order for the data stored in memory, col-major/row-major
// @param elemType : The type for the element.
// @param layout : This is identify for factor(A/B) or accumulator(C) for 16 bit element matrix.
// @param memoryAccess :  The memory operands which provide
// additional operands, maybe volatile/Aligned/Nontemporal/MakePointerAvailable
// /MakePointerVisible/NonPrivatePointer used by CooperativeMatrix Load/Store.
// @param vecVal : The contiguous elements made up of a vector to be loaded or stored.
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
void LowerCooperativeMatrix::cooperativeMatrixStoreInternal(Value *dataPtr, Value *stride, bool isColMajor,
                                                            Builder::CooperativeMatrixElementType elemType,
                                                            Builder::CooperativeMatrixLayout layout,
                                                            unsigned memoryAccess, Value *&vecVal,
                                                            const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  auto waveSize = m_pipelineState->getShaderWaveSize(getShaderStage(builder.GetInsertBlock()->getParent()));
  assert(waveSize == 32 || waveSize == 64);

  // Calc element offset in memory
  Type *elemTy = builder.transCooperativeMatrixElementType(elemType);
  const unsigned dataBitwidth = elemTy->getScalarSizeInBits();
  const unsigned addrSpace = dataPtr->getType()->getPointerAddressSpace();
  assert(addrSpace == ADDR_SPACE_LOCAL || addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER || addrSpace == ADDR_SPACE_GLOBAL);

  stride = builder.CreateExactSDiv(stride, builder.getInt32(dataBitwidth / 8));

  // calc memoryAccess
  bool isVolatile = memoryAccess & Builder::MemoryAccessVolatileMask;
  bool isCoherent = memoryAccess & Builder::MemoryAccessCoherentMask;
  bool isTemporal = memoryAccess & Builder::MemoryAccessTemporalMask;

  auto props = getTypeProperties(elemType, layout);
  auto addrInfo = computeAddressing(layout, elemType, waveSize, stride, isColMajor, insertPos);

  vecVal = convCoopMatrixVecToFlatVec(builder, vecVal, elemType, layout);

  for (unsigned idx = 0; idx < props.numFlatElements; ++idx) {
    Value *offset = builder.CreateAdd(
        addrInfo.base, builder.CreateMul(addrInfo.macroStep, builder.getInt32(idx / addrInfo.microCount)));
    offset =
        builder.CreateAdd(offset, builder.CreateMul(addrInfo.microStep, builder.getInt32(idx % addrInfo.microCount)));
    Value *elePtr = builder.CreateGEP(elemTy, dataPtr, offset);
    Value *oneElement = builder.CreateExtractElement(vecVal, idx);
    StoreInst *st = builder.CreateStore(oneElement, elePtr, isVolatile);

    if (isCoherent && !(addrSpace == ADDR_SPACE_LOCAL && dataBitwidth < 32))
      st->setAtomic(AtomicOrdering::Unordered);
    if (isTemporal)
      st->setMetadata(LLVMContext::MD_nontemporal, MDNode::get(builder.getContext(), {}));
  }
}

// =====================================================================================================================
// Open-code cooperative matrix extract operation
//
// @param builder : builder to use
// @param matrix : the matrix from which to extract a component
// @param index : the index to be extracted
// @param elemType : the matrix element type
// @param layout : the matrix layout type
Value *LowerCooperativeMatrix::cooperativeMatrixExtract(BuilderCommon &builder, Value *matrix, Value *index,
                                                        Builder::CooperativeMatrixElementType elemType,
                                                        Builder::CooperativeMatrixLayout layout) {
  Value *vec = convCoopMatrixVecToFlatVec(builder, matrix, elemType, layout);

  // This is a hacky workaround to the fact that for SPV_NV_cooperative_matrix, we have to support matrix length as
  // a specialization constant even though, at the time of specialization constant lowering, we don't yet know the
  // wave size. We should remove this once a healther KHR extension has been released.
  if (layout == BuilderCommon::CooperativeMatrixLayout::AccumulatorMatrixLayout &&
      m_pipelineState->getShaderWaveSize(m_shaderStage) == 64) {
    unsigned length = cast<FixedVectorType>(vec->getType())->getNumElements();
    index = builder.CreateAnd(index, builder.getInt32(length - 1));
  }

  return builder.CreateExtractElement(vec, index);
}

// =====================================================================================================================
// Open-code cooperative matrix insert operation
//
// @param builder : builder to use
// @param matrix : the matrix into which to insert a component
// @param value : the value to be inserted
// @param index : the index to be inserted
// @param elemType : the matrix element type
// @param layout : the matrix layout type
Value *LowerCooperativeMatrix::cooperativeMatrixInsert(BuilderCommon &builder, Value *matrix, Value *value,
                                                       Value *index, Builder::CooperativeMatrixElementType elemType,
                                                       Builder::CooperativeMatrixLayout layout) {
  Value *vec = convCoopMatrixVecToFlatVec(builder, matrix, elemType, layout);

  // This is a hacky workaround to the fact that for SPV_NV_cooperative_matrix, we have to support matrix length as
  // a specialization constant even though, at the time of specialization constant lowering, we don't yet know the
  // wave size. We should remove this once a healther KHR extension has been released.
  if (layout == BuilderCommon::CooperativeMatrixLayout::AccumulatorMatrixLayout &&
      m_pipelineState->getShaderWaveSize(m_shaderStage) == 64) {
    unsigned length = cast<FixedVectorType>(vec->getType())->getNumElements();
    Value *outOfBounds = builder.CreateICmpUGE(index, builder.getInt32(length));
    index = builder.CreateAnd(index, builder.getInt32(length - 1));
    Value *newVec = builder.CreateInsertElement(vec, value, index);
    vec = builder.CreateSelect(outOfBounds, vec, newVec);
  } else {
    vec = builder.CreateInsertElement(vec, value, index);
  }

  return convFlatVecToCoopMatrixVec(builder, vec, elemType, layout);
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
                                                                Builder::CooperativeMatrixElementType srcElemType,
                                                                Builder::CooperativeMatrixElementType dstElemType,
                                                                const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = nullptr;
  const unsigned vecSize = cast<FixedVectorType>(source->getType())->getNumElements();
  Type *dstType = FixedVectorType::get(builder.transCooperativeMatrixElementType(dstElemType), vecSize);

  if ((srcElemType == Builder::CooperativeMatrixElementType::Float16 ||
       srcElemType == Builder::CooperativeMatrixElementType::Float32) &&
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
// Create cooperative matrix conversion.
// Element-wise-conversion
// @param castOp : The cast Opcode.
// @param source : The source cooperative matrix.
// @param srcElemType : Source matrix's element type.
// @param dstElemType : Destination matrix's element type.
// @param srcLayout : Layout for source matrix
// @param dstLayout : Layout for destination matrix
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixConvert(CastInst::CastOps castOp, Value *source,
                                                        Builder::CooperativeMatrixElementType srcElemType,
                                                        Builder::CooperativeMatrixElementType dstElemType,
                                                        Builder::CooperativeMatrixLayout srcLayout,
                                                        Builder::CooperativeMatrixLayout dstLayout,
                                                        const Twine &instName, Instruction *insertPos) {
  assert(source->getType()->isVectorTy());
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = nullptr;
  Value *threadId = getLaneNumber(builder);

  if (castOp == 0) { // Only reshape on 16bits, not do convert
    if ((srcLayout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout) &&
        (dstLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout)) {
      // After mulAdd, the type for the matrix waiting to reshape is 8*float here
      const unsigned vecNums = cast<FixedVectorType>(source->getType())->getNumElements();
      source = builder.CreateBitCast(source, FixedVectorType::get(builder.getInt32Ty(), vecNums));
    }
    resultValue = cooperativeMatrixReshape16BitElementGfx1011(source, srcElemType, srcLayout, dstLayout, threadId,
                                                              instName, insertPos);
  } else {
    unsigned numSrcBit = builder.transCooperativeMatrixElementType(srcElemType)->getScalarSizeInBits();
    unsigned numDstBit = builder.transCooperativeMatrixElementType(dstElemType)->getScalarSizeInBits();

    // Step 1: Some cases need change the layout due to different element types before conversion.
    if ((numSrcBit < numDstBit) && (srcLayout != dstLayout)) {
      // Need Reshape from A/B layout to C/D layout
      // This interface will do cooperativeVecToflatVec internally except 8bit reshape.
      source = cooperativeMatrixReshapeBeforeConvert(source, srcElemType, dstElemType, srcLayout, dstLayout, instName,
                                                     insertPos);
    } else {
      // For 16bit->32bit on Gfx11, no reshape needed as it will always in 	AccumulatorMatrixLayout
      source = convCoopMatrixVecToFlatVec(builder, source, srcElemType, srcLayout);
    }

    // Step 2: Just do flatElement conversion without any layout change.
    resultValue = cooperativeMatrixConvertInternal(castOp, source, srcElemType, dstElemType, instName, insertPos);

    // Step 3: Some cases need change the layout due to different element types after conversion.
    if ((numSrcBit > numDstBit) && (srcLayout != dstLayout)) {
      // All these reshape interfaces will return N*packetTy.
      // Need Reshape from A/B layout to C/D layout
      resultValue = cooperativeMatrixReshapeAfterConvert(resultValue, srcElemType, dstElemType, srcLayout, dstLayout,
                                                         instName, insertPos);
    } else {
      resultValue = convFlatVecToCoopMatrixVec(builder, resultValue, dstElemType, dstLayout);
    }
  }
  return resultValue;
}

// =====================================================================================================================
// Create cooperative matrix binary operation
//
// @param coopMatArithOp : The cooperative matrix arithmetic operation to perform.
// @param lhs : The first operand and it can be a scalar or a cooperative matrix.
// @param rhs : The second operand and it should be a cooperative matrix.
// @param elemType : Element type for the matrix.
// @param layout : Layout for the matrix.
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixBinaryOp(Builder::CooperativeMatrixArithOp coopMatArithOp, Value *lhs,
                                                         Value *rhs, Builder::CooperativeMatrixElementType elemType,
                                                         Builder::CooperativeMatrixLayout layout, const Twine &instName,
                                                         Instruction *insertPos) {
  assert(lhs->getType()->isVectorTy() && lhs->getType() == rhs->getType() || rhs->getType()->isVectorTy());
  Value *vcResult;
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  lhs = convCoopMatrixVecToFlatVec(builder, lhs, elemType, layout);
  rhs = convCoopMatrixVecToFlatVec(builder, rhs, elemType, layout);
  switch (coopMatArithOp) {
  case Builder::CooperativeMatrixArithOp::IAdd:
    vcResult = builder.CreateAdd(lhs, rhs);
    break;
  case Builder::CooperativeMatrixArithOp::FAdd:
    vcResult = builder.CreateFAdd(lhs, rhs);
    break;
  case Builder::CooperativeMatrixArithOp::ISub:
    vcResult = builder.CreateSub(lhs, rhs);
    break;
  case Builder::CooperativeMatrixArithOp::FSub:
    vcResult = builder.CreateFSub(lhs, rhs);
    break;
  case Builder::CooperativeMatrixArithOp::IMul:
    vcResult = builder.CreateMul(lhs, rhs);
    break;
  case Builder::CooperativeMatrixArithOp::FMul:
    vcResult = builder.CreateFMul(lhs, rhs);
    break;
  case Builder::CooperativeMatrixArithOp::FDiv:
    vcResult = builder.CreateFDiv(lhs, rhs);
    break;
  case Builder::CooperativeMatrixArithOp::SDiv:
    vcResult = builder.CreateSDiv(lhs, rhs);
    break;
  case Builder::CooperativeMatrixArithOp::UDiv:
    vcResult = builder.CreateUDiv(lhs, rhs);
    break;
  default:
    llvm_unreachable("unsupported binary operation for cooprative matrix!"); // Rem/Mod is not supported currently.
  }

  Value *coopMatResult = convFlatVecToCoopMatrixVec(builder, vcResult, elemType, layout);
  return coopMatResult;
}

// =====================================================================================================================
// Create cooperative matrix MatrixTimesScalar operation
//
// @param matrix : The first operand and it should be a cooperative matrix.
// @param scalar : The second operand and it should be a scalar.
// @param elemType : The component type of the matrix.
// @param layout : Identify whether it's A/B or C/D
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::coopMatrixTimesScalar(Value *matrix, Value *scalar,
                                                     Builder::CooperativeMatrixElementType elemType,
                                                     Builder::CooperativeMatrixLayout layout, const Twine &instName,
                                                     Instruction *insertPos) {
  assert(matrix->getType()->getScalarType()->isIntegerTy() || matrix->getType()->getScalarType()->isFloatTy());
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  Value *vcFlat = convCoopMatrixVecToFlatVec(builder, matrix, elemType, layout);
  const unsigned numElems = cast<FixedVectorType>(vcFlat->getType())->getNumElements();
  auto splat = builder.CreateVectorSplat(numElems, scalar);
  Value *vcFlatResult;
  if ((elemType == Builder::CooperativeMatrixElementType::Float16) ||
      (elemType == Builder::CooperativeMatrixElementType::Float32)) {
    vcFlatResult = builder.CreateFMul(vcFlat, splat);
  } else {
    vcFlatResult = builder.CreateMul(vcFlat, splat);
  }
  Value *coopMatResult = convFlatVecToCoopMatrixVec(builder, vcFlatResult, elemType, layout);
  return coopMatResult;
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
    Value *source, Builder::CooperativeMatrixElementType srcElemType, Builder::CooperativeMatrixLayout srcLayout,
    Builder::CooperativeMatrixLayout dstLayout, Value *threadId, const Twine &instName, Instruction *insertPos) {
  assert(srcElemType == Builder::CooperativeMatrixElementType::Float16 ||
         srcElemType == Builder::CooperativeMatrixElementType::Int16);
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
  if (srcLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) { // From A/B to C/D for 16bit element
    Type *packedTy =
        (srcElemType == Builder::CooperativeMatrixElementType::Float16) ? builder.getFloatTy() : builder.getInt32Ty();
    if (dstLayout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout) {
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
      if (srcElemType == Builder::CooperativeMatrixElementType::Float16) {
        resultValue = builder.CreateBitCast(resultValue, FixedVectorType::get(builder.getFloatTy(), shiftVecNums),
                                            instName); // Bitcast to 8*bit32 for wave32 and 4*bit32 for wave64
        resultValue = builder.CreateShuffleVector(resultValue, PoisonValue::get(resultValue->getType()),
                                                  {0, 1, 2, 3, 4, 5, 6, 7});
      }
    } else if (dstLayout ==
               Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) { // Emulation on NAVI2X
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
  } else if (srcLayout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout) {
    if (dstLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) {
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
      if (srcElemType == Builder::CooperativeMatrixElementType::Float16) {
        matrix = builder.CreateBitCast(matrix, FixedVectorType::get(builder.getFloatTy(), 8)); //->8*f32
      }
      resultValue = matrix;
    }
  } else if (srcLayout == Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) {
    if (dstLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) { // NAVI2X:16bit reshape C/D->A/B
      // C/D: LANE0: {1_0:0_0 5_0:4_0 9_0:8_0 13_0:12_0} LANE16:{3_0:2_0 7_0:6_0 11_0:10_0 15_0:14_0}===>
      // A/B: LANE0: {1_0:0_0 3_0:2_0 5_0:4:0....15_0:14_0}  LANE16=LANE0
      Type *packedTy =
          (srcElemType == Builder::CooperativeMatrixElementType::Float16) ? builder.getFloatTy() : builder.getInt32Ty();
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
    Value *source, Builder::CooperativeMatrixElementType srcElemType, Builder::CooperativeMatrixLayout srcLayout,
    const Twine &instName, Instruction *insertPos) {

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = nullptr;
  auto waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  Value *threadId = getLaneNumber(builder);
  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

  if (srcLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) {
    assert(srcElemType == Builder::CooperativeMatrixElementType::Int8);
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
  } else if (srcLayout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout ||
             srcLayout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout) {
    //
    assert(srcElemType == Builder::CooperativeMatrixElementType::Int32 ||
           srcElemType == Builder::CooperativeMatrixElementType::Float32);
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
    Value *source, Builder::CooperativeMatrixElementType srcElemType, Builder::CooperativeMatrixElementType dstElemType,
    Builder::CooperativeMatrixLayout layout, Value *isEvenGroup, const Twine &instName, Instruction *insertPos) {
  // 1. After convert from f32->f16: change the layout from 32bit layout to 16bit layout on Accumulator on gfx10.
  // 2. Before convert from f16->f32: change the layout from 16bit layout to 32bit layout on Accumulator on gfx10

  // For 1st case:  lane0:{0_0 2_0 4_0..14_0} lane16:{1_0 3_0 5_0...15_0} lane32=lane0 lane48=lane16(8*half) ==>
  //               lane0:{1_0:0_0 5_0:4_0 ....} lane16:{3_0:2_0 7_0:6_0..} (4*float)
  // For 2nd case: lane0:{1_0:0_0 5_0:4_0 ....} lane16:{3_0:2_0 7_0:6_0..}(4*float) ==>
  //              lane0:{0_0 2_0 4_0..14_0} lane16:{1_0 3_0 5_0...15_0}(8*half)
  // From the implementation side, it's same which only exchange off-diaglog element between {2_0:0_0} and {3_0:1_0}(1st
  // case)
  //                              or {1_0:0_0} and {3_0:2_0}(2nd case)
  assert(layout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout ||
         layout == Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout);
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  Value *resultValue = nullptr;
  if (dstElemType == Builder::CooperativeMatrixElementType::Float16 ||
      dstElemType == Builder::CooperativeMatrixElementType::Int16) {
    source = builder.CreateBitCast(source, FixedVectorType::get(builder.getInt32Ty(), 4));
  } else if (dstElemType == Builder::CooperativeMatrixElementType::Float32 ||
             dstElemType == Builder::CooperativeMatrixElementType::Int32) {
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

  if (srcElemType == Builder::CooperativeMatrixElementType::Float16 &&
      (dstElemType == Builder::CooperativeMatrixElementType::Float32 ||
       dstElemType == Builder::CooperativeMatrixElementType::Int32)) {
    resultValue =
        builder.CreateBitCast(resultValue, FixedVectorType::get(builder.getHalfTy(), 16)); // 2nd case:before convert
  } else {
    resultValue =
        builder.CreateBitCast(resultValue, FixedVectorType::get(builder.getFloatTy(), 4)); // 1st case:after convert
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
                                                                     Builder::CooperativeMatrixElementType srcElemType,
                                                                     Builder::CooperativeMatrixElementType dstElemType,
                                                                     Builder::CooperativeMatrixLayout srcLayout,
                                                                     Builder::CooperativeMatrixLayout dstLayout,
                                                                     const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = source;

  Value *threadId = getLaneNumber(builder);
  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

  if (srcElemType == Builder::CooperativeMatrixElementType::Float16 ||
      srcElemType == Builder::CooperativeMatrixElementType::Int16) {
    if (srcLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout &&
        dstLayout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout) {
      resultValue = cooperativeMatrixReshape16BitElementGfx1011(source, srcElemType, srcLayout, dstLayout, threadId,
                                                                "reshapeFactorToAcc", insertPos);
      resultValue = convCoopMatrixVecToFlatVec(builder, resultValue, srcElemType, dstLayout);
    } else if (srcLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout &&
               dstLayout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout) {
      resultValue = cooperativeMatrixReshape16BitElementGfx1011(
          source, srcElemType, srcLayout, Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout, threadId,
          "reshapeFactorToAcc", insertPos);
      resultValue = cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(
          resultValue, srcElemType, dstElemType, dstLayout, isEvenGroup, "beforef16tof32", insertPos);
      resultValue = convCoopMatrixVecToFlatVec(builder, resultValue, srcElemType, dstLayout);
    } else if (srcLayout == Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout &&
               dstLayout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout) {
      resultValue = cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(source, srcElemType, dstElemType, dstLayout,
                                                                           isEvenGroup, "beforef16tof32", insertPos);
      resultValue = convCoopMatrixVecToFlatVec(builder, resultValue, srcElemType, dstLayout);
    } else {
      llvm_unreachable("Unsupported layout!");
    }
  } else if (srcElemType == Builder::CooperativeMatrixElementType::Int8) {
    // 8bit already return the N*flatType, it's unnecessary to call convCoopMatrixVecToFlatVec
    if (srcLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) {
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
                                                                    Builder::CooperativeMatrixElementType srcElemType,
                                                                    Builder::CooperativeMatrixElementType dstElemType,
                                                                    Builder::CooperativeMatrixLayout srcLayout,
                                                                    Builder::CooperativeMatrixLayout dstLayout,
                                                                    const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *resultValue = source;

  Value *threadId = getLaneNumber(builder);
  Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
  Value *isEvenGroup = builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

  if (dstElemType == Builder::CooperativeMatrixElementType::Float16 ||
      dstElemType == Builder::CooperativeMatrixElementType::Int16) {
    if (srcLayout == Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout &&
        dstLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) {
      // It needs to convert 16bit*8 into 32bit*8(high 16bit will be unused) as
      // the input for reshape interface will be 32bit*8 keeping compatibility for reshape+muladd+reshape case.
      resultValue =
          builder.CreateShuffleVector(resultValue, PoisonValue::get(source->getType()), {0, 1, 2, 3, 4, 5, 6, 7});
      resultValue = builder.CreateBitCast(resultValue, FixedVectorType::get(builder.getInt16Ty(), 8));
      resultValue = builder.CreateZExt(resultValue, FixedVectorType::get(builder.getInt32Ty(), 8), "zext");
      resultValue = cooperativeMatrixReshape16BitElementGfx1011(resultValue, dstElemType, srcLayout, dstLayout,
                                                                threadId, "reshapeAccToFactor", insertPos);
    } else if (srcLayout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout &&
               dstLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) {
      resultValue = cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(source, srcElemType, dstElemType, srcLayout,
                                                                           isEvenGroup, "afterf32tof16", insertPos);
      resultValue = cooperativeMatrixReshape16BitElementGfx1011(
          resultValue, dstElemType, Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout, dstLayout,
          threadId, "reshapeAccToFactor", insertPos);
    } else if (srcLayout == Builder::CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout &&
               dstLayout == Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout) {
      resultValue = cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(source, srcElemType, dstElemType, srcLayout,
                                                                           isEvenGroup, "afterf32tof16", insertPos);
    } else {
      llvm_unreachable("Unsupported elemtype!");
    }
  } else if (dstElemType == Builder::CooperativeMatrixElementType::Int8) {
    if (dstLayout == Builder::CooperativeMatrixLayout::FactorMatrixLayout) { // gfx10/gfx11: 32bit->8bit
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
// Create cooperative matrix transpose operation
//
// @param matrix : The first operand and it should be a cooperative matrix.
// @param elemType : The component type of the matrix.
// @param srcLayout: Identify whether it's A/B or C/D
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixTranspose(llvm::Value *matrix,
                                                          Builder::CooperativeMatrixElementType elemType,
                                                          Builder::CooperativeMatrixLayout srcLayout,
                                                          const Twine &instName, llvm::Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

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

  if (elemType == Builder::CooperativeMatrixElementType::Int8) {
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
  } else if (elemType == Builder::CooperativeMatrixElementType::Int16 ||
             elemType == Builder::CooperativeMatrixElementType::Float16) {
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
    if (elemType == Builder::CooperativeMatrixElementType::Float16) {
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
  return resultValue;
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
// Create cooperative matrix muladd operation
//
// @param matrixA : Factor cooperative matrix.
// @param matrixB : Factor cooperative matrix.
// @param matrixC : Accumulator cooperative matrix.
// @param isSignedA : Identify the signess for matrix A's element type
// @param isSignedB : Identify the signess for matrix B's element type
// @param isSat : SaturatingAccumulation for calculation
// @param accumElemType : The component type of the accumulator matrix.
// @param factorElemType : The component type of the factor matrix.
// @param matrixCLayout: The layout for matrix C/D.
// @param instName : Name to give instruction(s).
// @param insertPos : Where to insert the instruction
Value *LowerCooperativeMatrix::cooperativeMatrixMulAdd(llvm::Value *matrixA, llvm::Value *matrixB, llvm::Value *matrixC,
                                                       bool isSignedA, bool isSignedB, bool isSat,
                                                       Builder::CooperativeMatrixElementType accumElemType,
                                                       Builder::CooperativeMatrixElementType factorElemType,
                                                       const Twine &instName, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

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

    if (factorElemType == Builder::CooperativeMatrixElementType::Float16 ||
        factorElemType == Builder::CooperativeMatrixElementType::Int16) {
      unsigned factorFlatElemNum = 0;
      { factorFlatElemNum = 16; }
      Type *factorType =
          FixedVectorType::get(builder.transCooperativeMatrixElementType(factorElemType), factorFlatElemNum);
      matrixA = builder.CreateBitCast(matrixA, factorType);
      matrixB = builder.CreateBitCast(matrixB, factorType);
    } else if (factorElemType == Builder::CooperativeMatrixElementType::Int8) {
    } else {
      llvm_unreachable("Factor element type is not supported!");
    }

    if (accumElemType == Builder::CooperativeMatrixElementType::Float32 ||
        accumElemType == Builder::CooperativeMatrixElementType::Int32) {
      matrixC =
          waveSize == 64 ? builder.CreateShuffleVector(matrixC, ArrayRef<int>({0, 1, 2, 3}), "shuffleVector") : matrixC;
    } else if (accumElemType == Builder::CooperativeMatrixElementType::Float16 ||
               accumElemType == Builder::CooperativeMatrixElementType::Int16) {
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

    if (factorElemType == Builder::CooperativeMatrixElementType::Float16 &&
        accumElemType == Builder::CooperativeMatrixElementType::Float32) {
      matrixD = builder.CreateIntrinsic(matrixC->getType(), Intrinsic::amdgcn_wmma_f32_16x16x16_f16,
                                        {matrixA, matrixB, matrixC}, nullptr, instName);

    } else if (factorElemType == Builder::CooperativeMatrixElementType::Int8 &&
               accumElemType == Builder::CooperativeMatrixElementType::Int32) {
      matrixD = builder.CreateIntrinsic(
          matrixC->getType(), Intrinsic::amdgcn_wmma_i32_16x16x16_iu8,
          {builder.getInt1(isSignedA), matrixA, builder.getInt1(isSignedB), matrixB, matrixC, builder.getInt1(isSat)},
          nullptr, instName);

    } else if (factorElemType == Builder::CooperativeMatrixElementType::Float16 &&
               accumElemType == Builder::CooperativeMatrixElementType::Float16) {
      // Matrix convert to match intrinsic arguments: Wave32: float32*v8->half*v16
      // Wave64: float32*v4->half*v8
      matrixD = builder.CreateIntrinsic(matrixC->getType(), Intrinsic::amdgcn_wmma_f16_16x16x16_f16,
                                        {matrixA, matrixB, matrixC, builder.getInt1(isSat)}, nullptr, instName);
    } else {
      llvm_unreachable("The accumulator type is not supported.");
    }

    if (accumElemType == Builder::CooperativeMatrixElementType::Float16 ||
        accumElemType == Builder::CooperativeMatrixElementType::Int16) {
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
    return matrixD;
  } else { // Emulator on NAVI2X

    Type *packedTy = (factorElemType == Builder::CooperativeMatrixElementType::Float16) ? builder.getFloatTy()
                                                                                        : builder.getInt32Ty();
    Value *dotProductValue;

    Value *threadId = getLaneNumber(builder);
    Value *laneGroupIdx = builder.CreateUDiv(threadId, builder.getInt32(16));
    Value *isEvenGroup =
        builder.CreateICmpEQ(builder.CreateAnd(laneGroupIdx, builder.getInt32(1)), builder.getInt32(0));

    unsigned flags = (isSignedB << 1) | isSignedA;
    auto mapFuncReadLane = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                              ArrayRef<Value *> passthroughArgs) -> Value * {
      Type *const int32Ty = builder.getInt32Ty();

      return builder.CreateIntrinsic(int32Ty, Intrinsic::amdgcn_readlane, {mappedArgs[0], passthroughArgs[0]});
    };

    // matrixC is not reshaped for gfx10
    if (accumElemType == Builder::CooperativeMatrixElementType::Float32 ||
        accumElemType == Builder::CooperativeMatrixElementType::Int32) {
      dotProductValue = PoisonValue::get(FixedVectorType::get(packedTy, 8));
      for (unsigned idxc = 0; idxc < 8; ++idxc) {
        Value *rowlowgroup = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc * 2));
        Value *rowhighgroup = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc * 2 + 1));
        Value *rowData = builder.CreateSelect(isEvenGroup, rowlowgroup, rowhighgroup);
        Value *mulAB;
        Value *initAccumulator = builder.CreateExtractElement(matrixC, idxc);
        if (factorElemType == Builder::CooperativeMatrixElementType::Float16) {
          mulAB = createDotProductFp16Fp32(rowData, matrixB, initAccumulator, isSat, instName, insertPos);
        } else if (factorElemType == Builder::CooperativeMatrixElementType::Int16) {
          mulAB = createDotProductInt16Int32(rowData, matrixB, initAccumulator, flags, isSat, instName, insertPos);
        } else if (factorElemType == Builder::CooperativeMatrixElementType::Int8) {
          mulAB = createDotProductInt8Int32(rowData, matrixB, initAccumulator, flags, isSat, instName, insertPos);
        } else {
          llvm_unreachable("Unsupported element type!");
        }
        dotProductValue = builder.CreateInsertElement(dotProductValue, mulAB, idxc);
      }
    } else if (accumElemType == Builder::CooperativeMatrixElementType::Int16 ||
               accumElemType == Builder::CooperativeMatrixElementType::Float16) {
      dotProductValue =
          PoisonValue::get(FixedVectorType::get(builder.transCooperativeMatrixElementType(accumElemType), 8));
      // For gfx10, A*B:8*float32->16*half  C: no reshape for 16bit, still 16*half
      Value *colData = convCoopMatrixVecToFlatVec(builder, matrixB, factorElemType,
                                                  Builder::CooperativeMatrixLayout::FactorMatrixLayout);
      matrixC = convCoopMatrixVecToFlatVec(builder, matrixC, accumElemType,
                                           Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout);

      for (unsigned idxc = 0, accIdx = 0; idxc < 16; idxc += 4, accIdx += 2) {
        Value *rowData1Low = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc));
        Value *rowData2Low = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc + 1));
        Value *rowData1High = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc + 2));
        Value *rowData2High = builder.CreateMapToSimpleType(mapFuncReadLane, matrixA, builder.getInt32(idxc + 3));

        Value *rowData1 = builder.CreateSelect(isEvenGroup, rowData1Low, rowData1High);
        Value *rowData2 = builder.CreateSelect(isEvenGroup, rowData2Low, rowData2High);

        rowData1 = convCoopMatrixVecToFlatVec(builder, rowData1, factorElemType,
                                              Builder::CooperativeMatrixLayout::FactorMatrixLayout);
        rowData2 = convCoopMatrixVecToFlatVec(builder, rowData2, factorElemType,
                                              Builder::CooperativeMatrixLayout::FactorMatrixLayout);

        Value *mulAB1;
        Value *mulAB2;
        Value *accumulator1 = builder.CreateExtractElement(matrixC, accIdx);
        Value *accumulator2 = builder.CreateExtractElement(matrixC, accIdx + 1);

        if (accumElemType == Builder::CooperativeMatrixElementType::Float16) {
          mulAB1 = createDotProductFp16Fp16(rowData1, colData, accumulator1, isSat, instName, insertPos);
          mulAB2 = createDotProductFp16Fp16(rowData2, colData, accumulator2, isSat, instName, insertPos);
        } else {
          mulAB1 = createDotProductInt16Int16(rowData1, colData, accumulator1, flags, isSat, instName, insertPos);
          mulAB2 = createDotProductInt16Int16(rowData2, colData, accumulator2, flags, isSat, instName, insertPos);
        }
        dotProductValue = builder.CreateInsertElement(dotProductValue, mulAB1, accIdx);
        dotProductValue = builder.CreateInsertElement(dotProductValue, mulAB2, accIdx + 1);
      }

      dotProductValue = convFlatVecToCoopMatrixVec(builder, dotProductValue, accumElemType,
                                                   Builder::CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout);
    } else {
      llvm_unreachable("The accumulator type is not supported.");
    }
    return dotProductValue;
  }
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

  const unsigned compCount = cast<FixedVectorType>(vector1->getType())->getNumElements();
  Value *scalar = initAccumulator;
  auto intrinsicDot = Intrinsic::amdgcn_fdot2;
  for (unsigned i = 0; i < compCount; ++i) {
    Value *input1 = builder.CreateExtractElement(vector1, i);
    input1 = builder.CreateBitCast(input1, FixedVectorType::get(builder.getHalfTy(), 2));
    Value *input2 = builder.CreateExtractElement(vector2, i);
    input2 = builder.CreateBitCast(input2, FixedVectorType::get(builder.getHalfTy(), 2));
    scalar =
        builder.CreateIntrinsic(intrinsicDot, {}, {input1, input2, scalar, builder.getInt1(isSat)}, nullptr, instName);
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

  const bool isSigned = (flags & lgc::Builder::FirstVectorSigned);
  auto intrinsicDot = isSigned ? Intrinsic::amdgcn_sdot4 : Intrinsic::amdgcn_udot4;

  Value *scalar = builder.getInt32(0);
  const unsigned compCount = cast<FixedVectorType>(vector1->getType())->getNumElements();
  for (unsigned i = 0; i < compCount; ++i) {
    Value *input1 = builder.CreateExtractElement(vector1, i);
    Value *input2 = builder.CreateExtractElement(vector2, i);
    scalar =
        builder.CreateIntrinsic(intrinsicDot, {}, {input1, input2, scalar, builder.getInt1(false)}, nullptr, instName);
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

  const bool isSigned = (flags & lgc::Builder::FirstVectorSigned);
  auto intrinsicDot = isSigned ? Intrinsic::amdgcn_sdot2 : Intrinsic::amdgcn_udot2;

  Value *scalar = accumulator;
  const unsigned compCount = cast<FixedVectorType>(vector1->getType())->getNumElements();
  for (unsigned i = 0; i < compCount; ++i) {
    Value *input1 = builder.CreateExtractElement(vector1, i);
    input1 = builder.CreateBitCast(input1, FixedVectorType::get(builder.getInt16Ty(), 2));
    Value *input2 = builder.CreateExtractElement(vector2, i);
    input2 = builder.CreateBitCast(input2, FixedVectorType::get(builder.getInt16Ty(), 2));
    scalar =
        builder.CreateIntrinsic(intrinsicDot, {}, {input1, input2, scalar, builder.getInt1(isSat)}, nullptr, instName);
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
Value *LowerCooperativeMatrix::createDotProductInt16Int16(Value *vector1, Value *vector2, Value *accumulator,
                                                          unsigned flags, bool isSat, const Twine &instName,
                                                          Instruction *insertPos) {
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

  Type *targetTy = builder.getInt64Ty();
  // Emulate dot product with no HW support cases
  Value *scalar = builder.getInt64(0);
  for (unsigned elemIdx = 0; elemIdx < compCount; ++elemIdx) {
    Value *elem1 = builder.CreateExtractElement(vector1, elemIdx);
    elem1 = isSigned ? builder.CreateSExt(elem1, targetTy) : builder.CreateZExt(elem1, targetTy);
    Value *elem2 = builder.CreateExtractElement(vector2, elemIdx);
    elem2 = (isSigned && !isMixed) ? builder.CreateSExt(elem2, targetTy) : builder.CreateZExt(elem2, targetTy);
    Value *product = builder.CreateMul(elem1, elem2);
    scalar = builder.CreateAdd(product, scalar);
  }

  scalar = builder.CreateTrunc(scalar, builder.getInt32Ty());
  accumulator = builder.CreateTrunc(accumulator, builder.getInt32Ty());
  Intrinsic::ID addIntrinsic = isSigned ? Intrinsic::sadd_sat : Intrinsic::uadd_sat;
  scalar = builder.CreateBinaryIntrinsic(addIntrinsic, scalar, accumulator, nullptr, instName);

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  auto unsignedMax = (2ULL << (bitWidth - 1)) - 1;
  auto signedMax = unsignedMax >> 1;
  auto signedMin = -1ULL - signedMax;

  Value *minimum = nullptr, *maximum = nullptr;
  Value *isUnderflow = nullptr, *isOverflow = nullptr;
  if (isSigned) {
    scalar = builder.CreateSExt(scalar, builder.getInt64Ty());
    minimum = ConstantInt::getSigned(builder.getInt64Ty(), signedMin);
    maximum = ConstantInt::getSigned(builder.getInt64Ty(), signedMax);
    isUnderflow = builder.CreateICmpSLT(scalar, minimum);
    isOverflow = builder.CreateICmpSGT(scalar, maximum);
  } else {
    scalar = builder.CreateZExt(scalar, builder.getInt64Ty());
    minimum = builder.getInt64(0);
    maximum = builder.getInt64(unsignedMax);
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
// Get lane id.
// @param builder : The IR builder to create and insert IR instruction
Value *LowerCooperativeMatrix::getLaneNumber(BuilderBase &builder) {
  Value *result = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});
  if (m_pipelineState->getShaderWaveSize(m_shaderStage) == 64)
    result = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), result});
  return result;
}

} // namespace lgc
