/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  VertexFetch.cpp
 * @brief LLPC source file: contains implementation of class lgc::VertexFetch.
 ***********************************************************************************************************************
 */
#include "VertexFetch.h"
#include "SystemValues.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "lgc-vertex-fetch"

using namespace llvm;

namespace lgc {

#define VERTEX_FORMAT_UNDEFINED(_format)                                                                               \
  { _format, BUF_NUM_FORMAT_FLOAT, BUF_DATA_FORMAT_INVALID, 0, }

// Initializes info table of vertex component format map
const VertexCompFormatInfo VertexFetch::MVertexCompFormatInfo[] = {
    {0, 0, 0, BUF_DATA_FORMAT_INVALID},    // BUF_DATA_FORMAT_INVALID
    {1, 1, 1, BUF_DATA_FORMAT_8},          // BUF_DATA_FORMAT_8
    {2, 2, 1, BUF_DATA_FORMAT_16},         // BUF_DATA_FORMAT_16
    {2, 1, 2, BUF_DATA_FORMAT_8},          // BUF_DATA_FORMAT_8_8
    {4, 4, 1, BUF_DATA_FORMAT_32},         // BUF_DATA_FORMAT_32
    {4, 2, 2, BUF_DATA_FORMAT_16},         // BUF_DATA_FORMAT_16_16
    {4, 0, 0, BUF_DATA_FORMAT_10_11_11},   // BUF_DATA_FORMAT_10_11_11 (Packed)
    {4, 0, 0, BUF_DATA_FORMAT_11_11_10},   // BUF_DATA_FORMAT_11_11_10 (Packed)
    {4, 0, 0, BUF_DATA_FORMAT_10_10_10_2}, // BUF_DATA_FORMAT_10_10_10_2 (Packed)
    {4, 0, 0, BUF_DATA_FORMAT_2_10_10_10}, // BUF_DATA_FORMAT_2_10_10_10 (Packed)
    {4, 1, 4, BUF_DATA_FORMAT_8},          // BUF_DATA_FORMAT_8_8_8_8
    {8, 4, 2, BUF_DATA_FORMAT_32},         // BUF_DATA_FORMAT_32_32
    {8, 2, 4, BUF_DATA_FORMAT_16},         // BUF_DATA_FORMAT_16_16_16_16
    {12, 4, 3, BUF_DATA_FORMAT_32},        // BUF_DATA_FORMAT_32_32_32
    {16, 4, 4, BUF_DATA_FORMAT_32},        // BUF_DATA_FORMAT_32_32_32_32
};

const BufFormat VertexFetch::MVertexFormatMap[] = {
    // BUF_DATA_FORMAT
    //   BUF_NUM_FORMAT_UNORM
    //   BUF_NUM_FORMAT_SNORM
    //   BUF_NUM_FORMAT_USCALED
    //   BUF_NUM_FORMAT_SSCALED
    //   BUF_NUM_FORMAT_UINT
    //   BUF_NUM_FORMAT_SINT
    //   BUF_NUM_FORMAT_SNORM_NZ
    //   BUF_NUM_FORMAT_FLOAT

    // BUF_DATA_FORMAT_INVALID
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    // BUF_DATA_FORMAT_8
    BUF_FORMAT_8_UNORM,
    BUF_FORMAT_8_SNORM,
    BUF_FORMAT_8_USCALED,
    BUF_FORMAT_8_SSCALED,
    BUF_FORMAT_8_UINT,
    BUF_FORMAT_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    // BUF_DATA_FORMAT_16
    BUF_FORMAT_16_UNORM,
    BUF_FORMAT_16_SNORM,
    BUF_FORMAT_16_USCALED,
    BUF_FORMAT_16_SSCALED,
    BUF_FORMAT_16_UINT,
    BUF_FORMAT_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_FLOAT,

    // BUF_DATA_FORMAT_8_8
    BUF_FORMAT_8_8_UNORM,
    BUF_FORMAT_8_8_SNORM,
    BUF_FORMAT_8_8_USCALED,
    BUF_FORMAT_8_8_SSCALED,
    BUF_FORMAT_8_8_UINT,
    BUF_FORMAT_8_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    // BUF_DATA_FORMAT_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_UINT,
    BUF_FORMAT_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_FLOAT,

    // BUF_DATA_FORMAT_16_16
    BUF_FORMAT_16_16_UNORM,
    BUF_FORMAT_16_16_SNORM,
    BUF_FORMAT_16_16_USCALED,
    BUF_FORMAT_16_16_SSCALED,
    BUF_FORMAT_16_16_UINT,
    BUF_FORMAT_16_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_16_FLOAT,

    // BUF_DATA_FORMAT_10_11_11
    BUF_FORMAT_10_11_11_UNORM,
    BUF_FORMAT_10_11_11_SNORM,
    BUF_FORMAT_10_11_11_USCALED,
    BUF_FORMAT_10_11_11_SSCALED,
    BUF_FORMAT_10_11_11_UINT,
    BUF_FORMAT_10_11_11_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_10_11_11_FLOAT,

    // BUF_DATA_FORMAT_11_11_10
    BUF_FORMAT_11_11_10_UNORM,
    BUF_FORMAT_11_11_10_SNORM,
    BUF_FORMAT_11_11_10_USCALED,
    BUF_FORMAT_11_11_10_SSCALED,
    BUF_FORMAT_11_11_10_UINT,
    BUF_FORMAT_11_11_10_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_11_11_10_FLOAT,

    // BUF_DATA_FORMAT_10_10_10_2
    BUF_FORMAT_10_10_10_2_UNORM,
    BUF_FORMAT_10_10_10_2_SNORM,
    BUF_FORMAT_10_10_10_2_USCALED,
    BUF_FORMAT_10_10_10_2_SSCALED,
    BUF_FORMAT_10_10_10_2_UINT,
    BUF_FORMAT_10_10_10_2_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    // BUF_DATA_FORMAT_2_10_10_10
    BUF_FORMAT_2_10_10_10_UNORM,
    BUF_FORMAT_2_10_10_10_SNORM,
    BUF_FORMAT_2_10_10_10_USCALED,
    BUF_FORMAT_2_10_10_10_SSCALED,
    BUF_FORMAT_2_10_10_10_UINT,
    BUF_FORMAT_2_10_10_10_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    // BUF_DATA_FORMAT_8_8_8_8
    BUF_FORMAT_8_8_8_8_UNORM,
    BUF_FORMAT_8_8_8_8_SNORM,
    BUF_FORMAT_8_8_8_8_USCALED,
    BUF_FORMAT_8_8_8_8_SSCALED,
    BUF_FORMAT_8_8_8_8_UINT,
    BUF_FORMAT_8_8_8_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    // BUF_DATA_FORMAT_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_UINT,
    BUF_FORMAT_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_FLOAT,

    // BUF_DATA_FORMAT_16_16_16_16
    BUF_FORMAT_16_16_16_16_UNORM,
    BUF_FORMAT_16_16_16_16_SNORM,
    BUF_FORMAT_16_16_16_16_USCALED,
    BUF_FORMAT_16_16_16_16_SSCALED,
    BUF_FORMAT_16_16_16_16_UINT,
    BUF_FORMAT_16_16_16_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_16_16_16_FLOAT,

    // BUF_DATA_FORMAT_32_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_UINT,
    BUF_FORMAT_32_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_FLOAT,

    // BUF_DATA_FORMAT_32_32_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_32_UINT,
    BUF_FORMAT_32_32_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_32_FLOAT,

    // BUF_DATA_FORMAT_RESERVED_15
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
};

// =====================================================================================================================
//
// @param entryPoint : Entry-point of API vertex shader
// @param shaderSysValues : ShaderSystemValues object for getting vertex buffer pointer from
// @param pipelineState : Pipeline state
VertexFetch::VertexFetch(Function *entryPoint, ShaderSystemValues *shaderSysValues, PipelineState *pipelineState)
    : m_module(entryPoint->getParent()), m_context(&m_module->getContext()), m_shaderSysValues(shaderSysValues),
      m_pipelineState(pipelineState) {
  assert(getShaderStage(entryPoint) == ShaderStageVertex); // Must be vertex shader

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;
  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
  auto insertPos = entryPoint->begin()->getFirstInsertionPt();

  // VertexIndex = BaseVertex + VertexID
  if (builtInUsage.vertexIndex) {
    auto baseVertex = getFunctionArgument(entryPoint, entryArgIdxs.baseVertex);
    auto vertexId = getFunctionArgument(entryPoint, entryArgIdxs.vertexId);
    m_vertexIndex = BinaryOperator::CreateAdd(baseVertex, vertexId, "", &*insertPos);
  }

  // InstanceIndex = BaseInstance + InstanceID
  if (builtInUsage.instanceIndex) {
    m_baseInstance = getFunctionArgument(entryPoint, entryArgIdxs.baseInstance);
    m_instanceId = getFunctionArgument(entryPoint, entryArgIdxs.instanceId);
    m_instanceIndex = BinaryOperator::CreateAdd(m_baseInstance, m_instanceId, "", &*insertPos);
  }

  // Initialize default fetch values
  auto zero = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
  auto one = ConstantInt::get(Type::getInt32Ty(*m_context), 1);

  // Int8 (0, 0, 0, 1)
  m_fetchDefaults.int8 = ConstantVector::get({zero, zero, zero, one});

  // Int16 (0, 0, 0, 1)
  m_fetchDefaults.int16 = ConstantVector::get({zero, zero, zero, one});

  // Int (0, 0, 0, 1)
  m_fetchDefaults.int32 = ConstantVector::get({zero, zero, zero, one});

  // Int64 (0, 0, 0, 1)
  m_fetchDefaults.int64 = ConstantVector::get({zero, zero, zero, zero, zero, zero, zero, one});

  // Float16 (0, 0, 0, 1.0)
  const uint16_t float16One = 0x3C00;
  auto float16OneVal = ConstantInt::get(Type::getInt32Ty(*m_context), float16One);
  m_fetchDefaults.float16 = ConstantVector::get({zero, zero, zero, float16OneVal});

  // Float (0.0, 0.0, 0.0, 1.0)
  union {
    float f;
    unsigned u32;
  } floatOne = {1.0f};
  auto floatOneVal = ConstantInt::get(Type::getInt32Ty(*m_context), floatOne.u32);
  m_fetchDefaults.float32 = ConstantVector::get({zero, zero, zero, floatOneVal});

  // Double (0.0, 0.0, 0.0, 1.0)
  union {
    double d;
    unsigned u32[2];
  } doubleOne = {1.0};
  auto doubleOne0 = ConstantInt::get(Type::getInt32Ty(*m_context), doubleOne.u32[0]);
  auto doubleOne1 = ConstantInt::get(Type::getInt32Ty(*m_context), doubleOne.u32[1]);
  m_fetchDefaults.double64 = ConstantVector::get({zero, zero, zero, zero, zero, zero, doubleOne0, doubleOne1});
}

// =====================================================================================================================
// Executes vertex fetch operations based on the specified vertex input type and its location.
//
// @param inputTy : Type of vertex input
// @param location : Location of vertex input
// @param compIdx : Index used for vector element indexing
// @param insertPos : Where to insert vertex fetch instructions
Value *VertexFetch::run(Type *inputTy, unsigned location, unsigned compIdx, Instruction *insertPos) {
  Value *vertex = nullptr;

  // Get vertex input description for the given location
  const VertexInputDescription *description = m_pipelineState->findVertexInputDescription(location);

  // NOTE: If we could not find vertex input info matching this location, just return undefined value.
  if (!description)
    return UndefValue::get(inputTy);

  auto vbDesc = loadVertexBufferDescriptor(description->binding, insertPos);

  Value *vbIndex = nullptr;
  if (description->inputRate == VertexInputRateVertex)
    vbIndex = getVertexIndex(); // Use vertex index
  else {
    if (description->inputRate == VertexInputRateNone)
      vbIndex = m_baseInstance;
    else if (description->inputRate == VertexInputRateInstance)
      vbIndex = getInstanceIndex(); // Use instance index
    else {
      // There is a divisor.
      vbIndex = BinaryOperator::CreateUDiv(
          m_instanceId, ConstantInt::get(Type::getInt32Ty(*m_context), description->inputRate), "", insertPos);
      vbIndex = BinaryOperator::CreateAdd(vbIndex, m_baseInstance, "", insertPos);
    }
  }

  Value *vertexFetches[2] = {}; // Two vertex fetch operations might be required
  Value *vertexFetch = nullptr; // Coalesced vector by combining the results of two vertex fetch operations

  VertexFormatInfo formatInfo = getVertexFormatInfo(description);

  const bool is8bitFetch = (inputTy->getScalarSizeInBits() == 8);
  const bool is16bitFetch = (inputTy->getScalarSizeInBits() == 16);

  // Do the first vertex fetch operation
  addVertexFetchInst(vbDesc, formatInfo.numChannels, is16bitFetch, vbIndex, description->offset, description->stride,
                     formatInfo.dfmt, formatInfo.nfmt, insertPos, &vertexFetches[0]);

  // Do post-processing in certain cases
  std::vector<Constant *> shuffleMask;
  bool postShuffle = needPostShuffle(description, shuffleMask);
  bool patchA2S = needPatchA2S(description);
  if (postShuffle || patchA2S) {
    if (postShuffle) {
      // NOTE: If we are fetching a swizzled format, we have to add an extra "shufflevector" instruction to
      // get the components in the right order.
      assert(shuffleMask.empty() == false);
      vertexFetches[0] =
          new ShuffleVectorInst(vertexFetches[0], vertexFetches[0], ConstantVector::get(shuffleMask), "", insertPos);
    }

    if (patchA2S) {
      assert(cast<VectorType>(vertexFetches[0]->getType())->getNumElements() == 4);

      // Extract alpha channel: %a = extractelement %vf0, 3
      Value *alpha = ExtractElementInst::Create(vertexFetches[0], ConstantInt::get(Type::getInt32Ty(*m_context), 3), "",
                                                insertPos);

      if (formatInfo.nfmt == BufNumFormatSint) {
        // NOTE: For format "SINT 10_10_10_2", vertex fetches incorrectly return the alpha channel as
        // unsigned. We have to manually sign-extend it here by doing a "shl" 30 then an "ashr" 30.

        // %a = shl %a, 30
        alpha = BinaryOperator::CreateShl(alpha, ConstantInt::get(Type::getInt32Ty(*m_context), 30), "", insertPos);

        // %a = ashr %a, 30
        alpha = BinaryOperator::CreateAShr(alpha, ConstantInt::get(Type::getInt32Ty(*m_context), 30), "", insertPos);
      } else if (formatInfo.nfmt == BufNumFormatSnorm) {
        // NOTE: For format "SNORM 10_10_10_2", vertex fetches incorrectly return the alpha channel
        // as unsigned. We have to somehow remap the values { 0.0, 0.33, 0.66, 1.00 } to { 0.0, 1.0,
        // -1.0, -1.0 } respectively.

        // %a = bitcast %a to f32
        alpha = new BitCastInst(alpha, Type::getFloatTy(*m_context), "", insertPos);

        // %a = mul %a, 3.0f
        alpha = BinaryOperator::CreateFMul(alpha, ConstantFP::get(Type::getFloatTy(*m_context), 3.0f), "", insertPos);

        // %cond = ugt %a, 1.5f
        auto cond =
            new FCmpInst(insertPos, FCmpInst::FCMP_UGT, alpha, ConstantFP::get(Type::getFloatTy(*m_context), 1.5f), "");

        // %a = select %cond, -1.0f, pAlpha
        alpha = SelectInst::Create(cond, ConstantFP::get(Type::getFloatTy(*m_context), -1.0f), alpha, "", insertPos);

        // %a = bitcast %a to i32
        alpha = new BitCastInst(alpha, Type::getInt32Ty(*m_context), "", insertPos);
      } else if (formatInfo.nfmt == BufNumFormatSscaled) {
        // NOTE: For format "SSCALED 10_10_10_2", vertex fetches incorrectly return the alpha channel
        // as unsigned. We have to somehow remap the values { 0.0, 1.0, 2.0, 3.0 } to { 0.0, 1.0,
        // -2.0, -1.0 } respectively. We can perform the sign extension here by doing a "fptosi", "shl" 30,
        // "ashr" 30, and finally "sitofp".

        // %a = bitcast %a to float
        alpha = new BitCastInst(alpha, Type::getFloatTy(*m_context), "", insertPos);

        // %a = fptosi %a to i32
        alpha = new FPToSIInst(alpha, Type::getInt32Ty(*m_context), "", insertPos);

        // %a = shl %a, 30
        alpha = BinaryOperator::CreateShl(alpha, ConstantInt::get(Type::getInt32Ty(*m_context), 30), "", insertPos);

        // %a = ashr a, 30
        alpha = BinaryOperator::CreateAShr(alpha, ConstantInt::get(Type::getInt32Ty(*m_context), 30), "", insertPos);

        // %a = sitofp %a to float
        alpha = new SIToFPInst(alpha, Type::getFloatTy(*m_context), "", insertPos);

        // %a = bitcast %a to i32
        alpha = new BitCastInst(alpha, Type::getInt32Ty(*m_context), "", insertPos);
      } else
        llvm_unreachable("Should never be called!");

      // Insert alpha channel: %vf0 = insertelement %vf0, %a, 3
      vertexFetches[0] = InsertElementInst::Create(vertexFetches[0], alpha,
                                                   ConstantInt::get(Type::getInt32Ty(*m_context), 3), "", insertPos);
    }
  }

  // Do the second vertex fetch operation
  const bool secondFetch = needSecondVertexFetch(description);
  if (secondFetch) {
    unsigned numChannels = formatInfo.numChannels;
    unsigned dfmt = formatInfo.dfmt;

    if (description->dfmt == BufDataFormat64_64_64) {
      // Valid number of channels and data format have to be revised
      numChannels = 2;
      dfmt = BUF_DATA_FORMAT_32_32;
    }

    addVertexFetchInst(vbDesc, numChannels, is16bitFetch, vbIndex, description->offset + SizeOfVec4,
                       description->stride, dfmt, formatInfo.nfmt, insertPos, &vertexFetches[1]);
  }

  if (secondFetch) {
    // NOTE: If we performs vertex fetch operations twice, we have to coalesce result values of the two
    // fetch operations and generate a combined one.
    assert(vertexFetches[0] && vertexFetches[1]);
    assert(cast<VectorType>(vertexFetches[0]->getType())->getNumElements() == 4);

    unsigned compCount = cast<VectorType>(vertexFetches[1]->getType())->getNumElements();
    assert(compCount == 2 || compCount == 4); // Should be <2 x i32> or <4 x i32>

    if (compCount == 2) {
      // NOTE: We have to enlarge the second vertex fetch, from <2 x i32> to <4 x i32>. Otherwise,
      // vector shuffle operation could not be performed in that it requires the two vectors have
      // the same types.

      // %vf1 = shufflevector %vf1, %vf1, <0, 1, undef, undef>
      Constant *shuffleMask[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), 0), ConstantInt::get(Type::getInt32Ty(*m_context), 1),
          UndefValue::get(Type::getInt32Ty(*m_context)), UndefValue::get(Type::getInt32Ty(*m_context))};
      vertexFetches[1] =
          new ShuffleVectorInst(vertexFetches[1], vertexFetches[1], ConstantVector::get(shuffleMask), "", insertPos);
    }

    // %vf = shufflevector %vf0, %vf1, <0, 1, 2, 3, 4, 5, ...>
    shuffleMask.clear();
    for (unsigned i = 0; i < 4 + compCount; ++i)
      shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), i));
    vertexFetch =
        new ShuffleVectorInst(vertexFetches[0], vertexFetches[1], ConstantVector::get(shuffleMask), "", insertPos);
  } else
    vertexFetch = vertexFetches[0];

  // Finalize vertex fetch
  Type *basicTy = inputTy->isVectorTy() ? cast<VectorType>(inputTy)->getElementType() : inputTy;
  const unsigned bitWidth = basicTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  // Get default fetch values
  Constant *defaults = nullptr;

  if (basicTy->isIntegerTy()) {
    if (bitWidth == 8)
      defaults = m_fetchDefaults.int8;
    else if (bitWidth == 16)
      defaults = m_fetchDefaults.int16;
    else if (bitWidth == 32)
      defaults = m_fetchDefaults.int32;
    else {
      assert(bitWidth == 64);
      defaults = m_fetchDefaults.int64;
    }
  } else if (basicTy->isFloatingPointTy()) {
    if (bitWidth == 16)
      defaults = m_fetchDefaults.float16;
    else if (bitWidth == 32)
      defaults = m_fetchDefaults.float32;
    else {
      assert(bitWidth == 64);
      defaults = m_fetchDefaults.double64;
    }
  } else
    llvm_unreachable("Should never be called!");

  const unsigned defaultCompCount = cast<VectorType>(defaults->getType())->getNumElements();
  std::vector<Value *> defaultValues(defaultCompCount);

  for (unsigned i = 0; i < defaultValues.size(); ++i) {
    defaultValues[i] =
        ExtractElementInst::Create(defaults, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
  }

  // Get vertex fetch values
  const unsigned fetchCompCount =
      vertexFetch->getType()->isVectorTy() ? cast<VectorType>(vertexFetch->getType())->getNumElements() : 1;
  std::vector<Value *> fetchValues(fetchCompCount);

  if (fetchCompCount == 1)
    fetchValues[0] = vertexFetch;
  else {
    for (unsigned i = 0; i < fetchCompCount; ++i) {
      fetchValues[i] =
          ExtractElementInst::Create(vertexFetch, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
    }
  }

  // Construct vertex fetch results
  const unsigned inputCompCount = inputTy->isVectorTy() ? cast<VectorType>(inputTy)->getNumElements() : 1;
  const unsigned vertexCompCount = inputCompCount * (bitWidth == 64 ? 2 : 1);

  std::vector<Value *> vertexValues(vertexCompCount);

  // NOTE: Original component index is based on the basic scalar type.
  compIdx *= (bitWidth == 64 ? 2 : 1);

  // Vertex input might take values from vertex fetch values or default fetch values
  for (unsigned i = 0; i < vertexCompCount; i++) {
    if (compIdx + i < fetchCompCount)
      vertexValues[i] = fetchValues[compIdx + i];
    else if (compIdx + i < defaultCompCount)
      vertexValues[i] = defaultValues[compIdx + i];
    else {
      llvm_unreachable("Should never be called!");
      vertexValues[i] = UndefValue::get(Type::getInt32Ty(*m_context));
    }
  }

  if (vertexCompCount == 1)
    vertex = vertexValues[0];
  else {
    Type *vertexTy = VectorType::get(Type::getInt32Ty(*m_context), vertexCompCount);
    vertex = UndefValue::get(vertexTy);

    for (unsigned i = 0; i < vertexCompCount; ++i) {
      vertex = InsertElementInst::Create(vertex, vertexValues[i], ConstantInt::get(Type::getInt32Ty(*m_context), i), "",
                                         insertPos);
    }
  }

  if (is8bitFetch) {
    // NOTE: The vertex fetch results are represented as <n x i32> now. For 8-bit vertex fetch, we have to
    // convert them to <n x i8> and the 24 high bits is truncated.
    assert(inputTy->isIntOrIntVectorTy()); // Must be integer type

    Type *vertexTy = vertex->getType();
    Type *truncTy = Type::getInt8Ty(*m_context);
    truncTy = vertexTy->isVectorTy()
                  ? cast<Type>(VectorType::get(truncTy, cast<VectorType>(vertexTy)->getNumElements()))
                  : truncTy;
    vertex = new TruncInst(vertex, truncTy, "", insertPos);
  } else if (is16bitFetch) {
    // NOTE: The vertex fetch results are represented as <n x i32> now. For 16-bit vertex fetch, we have to
    // convert them to <n x i16> and the 16 high bits is truncated.
    Type *vertexTy = vertex->getType();
    Type *truncTy = Type::getInt16Ty(*m_context);
    truncTy = vertexTy->isVectorTy()
                  ? cast<Type>(VectorType::get(truncTy, cast<VectorType>(vertexTy)->getNumElements()))
                  : truncTy;
    vertex = new TruncInst(vertex, truncTy, "", insertPos);
  }

  return vertex;
}

// =====================================================================================================================
// Gets info from table according to vertex attribute format.
//
// @param inputDesc : Vertex input description
VertexFormatInfo VertexFetch::getVertexFormatInfo(const VertexInputDescription *inputDesc) {
  VertexFormatInfo info = {static_cast<BufNumFormat>(inputDesc->nfmt), static_cast<BufDataFormat>(inputDesc->dfmt), 1};
  switch (inputDesc->dfmt) {
  case BufDataFormat8_8:
  case BufDataFormat16_16:
  case BufDataFormat32_32:
    info.numChannels = 2;
    break;
  case BufDataFormat32_32_32:
  case BufDataFormat10_11_11:
  case BufDataFormat11_11_10:
    info.numChannels = 3;
    break;
  case BufDataFormat8_8_8_8:
  case BufDataFormat16_16_16_16:
  case BufDataFormat32_32_32_32:
  case BufDataFormat10_10_10_2:
  case BufDataFormat2_10_10_10:
    info.numChannels = 4;
    break;
  case BufDataFormat8_8_8_8_Bgra:
    info.numChannels = 4;
    info.dfmt = BufDataFormat8_8_8_8;
    break;
  case BufDataFormat2_10_10_10_Bgra:
    info.numChannels = 4;
    info.dfmt = BufDataFormat2_10_10_10;
    break;
  case BufDataFormat64:
    info.numChannels = 2;
    info.dfmt = BufDataFormat32_32;
    break;
  case BufDataFormat64_64:
  case BufDataFormat64_64_64:
  case BufDataFormat64_64_64_64:
    info.numChannels = 4;
    info.dfmt = BufDataFormat32_32_32_32;
    break;
  default:
    break;
  }
  return info;
}

// =====================================================================================================================
// Gets component info from table according to vertex buffer data format.
//
// @param dfmt : Date format of vertex buffer
const VertexCompFormatInfo *VertexFetch::getVertexComponentFormatInfo(unsigned dfmt) {
  assert(dfmt < sizeof(MVertexCompFormatInfo) / sizeof(MVertexCompFormatInfo[0]));
  return &MVertexCompFormatInfo[dfmt];
}

// =====================================================================================================================
// Maps separate buffer data and numeric formats to the combined buffer format
//
// @param dfmt : Data format
// @param nfmt : Numeric format
unsigned VertexFetch::mapVertexFormat(unsigned dfmt, unsigned nfmt) const {
  assert(dfmt < 16);
  assert(nfmt < 8);
  unsigned format = 0;

  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  if (gfxIp.major >= 10) {
    unsigned index = (dfmt * 8) + nfmt;
    assert(index < sizeof(MVertexFormatMap) / sizeof(MVertexFormatMap[0]));
    format = MVertexFormatMap[index];
  } else {
    CombineFormat formatOprd = {};
    formatOprd.bits.dfmt = dfmt;
    formatOprd.bits.nfmt = nfmt;
    format = formatOprd.u32All;
  }
  return format;
}

// =====================================================================================================================
// Loads vertex descriptor based on the specified vertex input location.
//
// @param binding : ID of vertex buffer binding
// @param insertPos : Where to insert instructions
Value *VertexFetch::loadVertexBufferDescriptor(unsigned binding, Instruction *insertPos) const {
  Value *idxs[] = {ConstantInt::get(Type::getInt64Ty(*m_context), 0, false),
                   ConstantInt::get(Type::getInt64Ty(*m_context), binding, false)};

  auto vbTablePtr = m_shaderSysValues->getVertexBufTablePtr();
  auto vbDescPtr = GetElementPtrInst::Create(nullptr, vbTablePtr, idxs, "", insertPos);
  vbDescPtr->setMetadata(MetaNameUniform, MDNode::get(vbDescPtr->getContext(), {}));
  auto vbDescTy = vbDescPtr->getType()->getPointerElementType();

  auto vbDesc = new LoadInst(vbDescTy, vbDescPtr, "", false, Align(16), insertPos);
  vbDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(vbDesc->getContext(), {}));

  return vbDesc;
}

// =====================================================================================================================
// Inserts instructions to do vertex fetch operations.
//
// @param vbDesc : Vertex buffer descriptor
// @param numChannels : Valid number of channels
// @param is16bitFetch : Whether it is 16-bit vertex fetch
// @param vbIndex : Index of vertex fetch in buffer
// @param offset : Vertex attribute offset (in bytes)
// @param stride : Vertex attribute stride (in bytes)
// @param dfmt : Date format of vertex buffer
// @param nfmt : Numeric format of vertex buffer
// @param insertPos : Where to insert instructions
// @param [out] ppFetch : Destination of vertex fetch
void VertexFetch::addVertexFetchInst(Value *vbDesc, unsigned numChannels, bool is16bitFetch, Value *vbIndex,
                                     unsigned offset, unsigned stride, unsigned dfmt, unsigned nfmt,
                                     Instruction *insertPos, Value **ppFetch) const {
  const VertexCompFormatInfo *formatInfo = getVertexComponentFormatInfo(dfmt);

  // NOTE: If the vertex attribute offset and stride are aligned on data format boundaries, we can do a vertex fetch
  // operation to read the whole vertex. Otherwise, we have to do vertex per-component fetch operations.
  if (((offset % formatInfo->vertexByteSize) == 0 && (stride % formatInfo->vertexByteSize) == 0) ||
      formatInfo->compDfmt == dfmt) {
    // NOTE: If the vertex attribute offset is greater than vertex attribute stride, we have to adjust both vertex
    // buffer index and vertex attribute offset accordingly. Otherwise, vertex fetch might behave unexpectedly.
    if (stride != 0 && offset > stride) {
      vbIndex = BinaryOperator::CreateAdd(vbIndex, ConstantInt::get(Type::getInt32Ty(*m_context), offset / stride), "",
                                          insertPos);
      offset = offset % stride;
    }

    // Do vertex fetch
    Value *args[] = {
        vbDesc,                                                                      // rsrc
        vbIndex,                                                                     // vindex
        ConstantInt::get(Type::getInt32Ty(*m_context), offset),                      // offset
        ConstantInt::get(Type::getInt32Ty(*m_context), 0),                           // soffset
        ConstantInt::get(Type::getInt32Ty(*m_context), mapVertexFormat(dfmt, nfmt)), // dfmt, nfmt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0)                            // glc, slc
    };

    StringRef suffix = "";
    Type *fetchTy = nullptr;

    if (is16bitFetch) {
      switch (numChannels) {
      case 1:
        suffix = ".f16";
        fetchTy = Type::getHalfTy(*m_context);
        break;
      case 2:
        suffix = ".v2f16";
        fetchTy = VectorType::get(Type::getHalfTy(*m_context), 2);
        break;
      case 3:
      case 4:
        suffix = ".v4f16";
        fetchTy = VectorType::get(Type::getHalfTy(*m_context), 4);
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
    } else {
      switch (numChannels) {
      case 1:
        suffix = ".i32";
        fetchTy = Type::getInt32Ty(*m_context);
        break;
      case 2:
        suffix = ".v2i32";
        fetchTy = VectorType::get(Type::getInt32Ty(*m_context), 2);
        break;
      case 3:
      case 4:
        suffix = ".v4i32";
        fetchTy = VectorType::get(Type::getInt32Ty(*m_context), 4);
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
    }

    Value *fetch = emitCall((Twine("llvm.amdgcn.struct.tbuffer.load") + suffix).str(), fetchTy, args, {}, insertPos);

    if (is16bitFetch) {
      // NOTE: The fetch values are represented by <n x i32>, so we will bitcast the float16 values to
      // int32 eventually.
      Type *bitCastTy = Type::getInt16Ty(*m_context);
      bitCastTy = numChannels == 1 ? bitCastTy : VectorType::get(bitCastTy, numChannels);
      fetch = new BitCastInst(fetch, bitCastTy, "", insertPos);

      Type *zExtTy = Type::getInt32Ty(*m_context);
      zExtTy = numChannels == 1 ? zExtTy : VectorType::get(zExtTy, numChannels);
      fetch = new ZExtInst(fetch, zExtTy, "", insertPos);
    }

    if (numChannels == 3) {
      // NOTE: If valid number of channels is 3, the actual fetch type should be revised from <4 x i32>
      // to <3 x i32>.
      Constant *shuffleMask[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 1),
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 2)};
      *ppFetch = new ShuffleVectorInst(fetch, fetch, ConstantVector::get(shuffleMask), "", insertPos);
    } else
      *ppFetch = fetch;
  } else {
    // NOTE: Here, we split the vertex into its components and do per-component fetches. The expectation
    // is that the vertex per-component fetches always match the hardware requirements.
    assert(numChannels == formatInfo->compCount);

    Value *compVbIndices[4] = {};
    unsigned compOffsets[4] = {};

    for (unsigned i = 0; i < formatInfo->compCount; ++i) {
      unsigned compOffset = offset + i * formatInfo->compByteSize;

      // NOTE: If the vertex attribute per-component offset is greater than vertex attribute stride, we have
      // to adjust both vertex buffer index and vertex per-component offset accordingly. Otherwise, vertex
      // fetch might behave unexpectedly.
      if (stride != 0 && compOffset > stride) {
        compVbIndices[i] = BinaryOperator::CreateAdd(
            vbIndex, ConstantInt::get(Type::getInt32Ty(*m_context), compOffset / stride), "", insertPos);
        compOffsets[i] = compOffset % stride;
      } else {
        compVbIndices[i] = vbIndex;
        compOffsets[i] = compOffset;
      }
    }

    Type *fetchTy = VectorType::get(Type::getInt32Ty(*m_context), numChannels);
    Value *fetch = UndefValue::get(fetchTy);

    // Do vertex per-component fetches
    for (unsigned i = 0; i < formatInfo->compCount; ++i) {
      Value *args[] = {
          vbDesc,                                                                                      // rsrc
          compVbIndices[i],                                                                            // vindex
          ConstantInt::get(Type::getInt32Ty(*m_context), compOffsets[i]),                              // offset
          ConstantInt::get(Type::getInt32Ty(*m_context), 0),                                           // soffset
          ConstantInt::get(Type::getInt32Ty(*m_context), mapVertexFormat(formatInfo->compDfmt, nfmt)), // dfmt, nfmt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0)                                            // glc, slc
      };

      Value *compFetch = nullptr;
      if (is16bitFetch) {
        compFetch = emitCall("llvm.amdgcn.struct.tbuffer.load.f16", Type::getHalfTy(*m_context), args, {}, insertPos);

        compFetch = new BitCastInst(compFetch, Type::getInt16Ty(*m_context), "", insertPos);
        compFetch = new ZExtInst(compFetch, Type::getInt32Ty(*m_context), "", insertPos);
      } else {
        compFetch = emitCall("llvm.amdgcn.struct.tbuffer.load.i32", Type::getInt32Ty(*m_context), args, {}, insertPos);
      }

      fetch =
          InsertElementInst::Create(fetch, compFetch, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
    }

    *ppFetch = fetch;
  }
}

// =====================================================================================================================
// Checks whether post shuffle is required for vertex fetch oepration.
//
// @param inputDesc : Vertex input description
// @param [out] shuffleMask : Vector shuffle mask
bool VertexFetch::needPostShuffle(const VertexInputDescription *inputDesc, std::vector<Constant *> &shuffleMask) const {
  bool needShuffle = false;

  switch (inputDesc->dfmt) {
  case BufDataFormat8_8_8_8_Bgra:
  case BufDataFormat2_10_10_10_Bgra:
    shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 2));
    shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 1));
    shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));
    shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 3));
    needShuffle = true;
    break;
  default:
    break;
  }

  return needShuffle;
}

// =====================================================================================================================
// Checks whether patching 2-bit signed alpha channel is required for vertex fetch operation.
//
// @param inputDesc : Vertex input description
bool VertexFetch::needPatchA2S(const VertexInputDescription *inputDesc) const {
  bool needPatch = false;

  if (inputDesc->dfmt == BufDataFormat2_10_10_10 || inputDesc->dfmt == BufDataFormat2_10_10_10_Bgra) {
    if (inputDesc->nfmt == BufNumFormatSnorm || inputDesc->nfmt == BufNumFormatSscaled ||
        inputDesc->nfmt == BufNumFormatSint)
      needPatch = m_pipelineState->getTargetInfo().getGfxIpVersion().major < 9;
  }

  return needPatch;
}

// =====================================================================================================================
// Checks whether the second vertex fetch operation is required (particularly for certain 64-bit typed formats).
//
// @param inputDesc : Vertex input description
bool VertexFetch::needSecondVertexFetch(const VertexInputDescription *inputDesc) const {
  return inputDesc->dfmt == BufDataFormat64_64_64 || inputDesc->dfmt == BufDataFormat64_64_64_64;
}

} // namespace lgc
