/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcVertexFetch.cpp
 * @brief LLPC source file: contains implementation of class Llpc::VertexFetch.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-vertex-fetch"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "llpcDebug.h"
#include "llpcPipelineState.h"
#include "llpcSystemValues.h"
#include "llpcTargetInfo.h"
#include "llpcVertexFetch.h"

using namespace llvm;

namespace Llpc
{

#define VERTEX_FORMAT_UNDEFINED(_format) \
{ \
    _format, \
    BUF_NUM_FORMAT_FLOAT, \
    BUF_DATA_FORMAT_INVALID, \
    0, \
}

// Initializes info table of vertex component format map
const VertexCompFormatInfo VertexFetch::m_vertexCompFormatInfo[] =
{
    { 0,  0, 0, BUF_DATA_FORMAT_INVALID     }, // BUF_DATA_FORMAT_INVALID
    { 1,  1, 1, BUF_DATA_FORMAT_8           }, // BUF_DATA_FORMAT_8
    { 2,  2, 1, BUF_DATA_FORMAT_16          }, // BUF_DATA_FORMAT_16
    { 2,  1, 2, BUF_DATA_FORMAT_8           }, // BUF_DATA_FORMAT_8_8
    { 4,  4, 1, BUF_DATA_FORMAT_32          }, // BUF_DATA_FORMAT_32
    { 4,  2, 2, BUF_DATA_FORMAT_16          }, // BUF_DATA_FORMAT_16_16
    { 4,  0, 0, BUF_DATA_FORMAT_10_11_11    }, // BUF_DATA_FORMAT_10_11_11 (Packed)
    { 4,  0, 0, BUF_DATA_FORMAT_11_11_10    }, // BUF_DATA_FORMAT_11_11_10 (Packed)
    { 4,  0, 0, BUF_DATA_FORMAT_10_10_10_2  }, // BUF_DATA_FORMAT_10_10_10_2 (Packed)
    { 4,  0, 0, BUF_DATA_FORMAT_2_10_10_10  }, // BUF_DATA_FORMAT_2_10_10_10 (Packed)
    { 4,  1, 4, BUF_DATA_FORMAT_8           }, // BUF_DATA_FORMAT_8_8_8_8
    { 8,  4, 2, BUF_DATA_FORMAT_32          }, // BUF_DATA_FORMAT_32_32
    { 8,  2, 4, BUF_DATA_FORMAT_16          }, // BUF_DATA_FORMAT_16_16_16_16
    { 12, 4, 3, BUF_DATA_FORMAT_32          }, // BUF_DATA_FORMAT_32_32_32
    { 16, 4, 4, BUF_DATA_FORMAT_32          }, // BUF_DATA_FORMAT_32_32_32_32
};

#if LLPC_BUILD_GFX10
const BufFormat VertexFetch::m_vertexFormatMap[] =
{
    // BUF_DATA_FORMAT
    //   BUF_NUM_FORMAT_UNORM
    //   BUF_NUM_FORMAT_SNORM
    //   BUF_NUM_FORMAT_USCALED
    //   BUF_NUM_FORMAT_SSCALED
    //   BUF_NUM_FORMAT_UINT
    //   BUF_NUM_FORMAT_SINT
    //   BUF_NUM_FORMAT_SNORM_NZ
    //   BUF_NUM_FORMAT_FLOAT

    //BUF_DATA_FORMAT_INVALID
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_8
    BUF_FORMAT_8_UNORM,
    BUF_FORMAT_8_SNORM,
    BUF_FORMAT_8_USCALED,
    BUF_FORMAT_8_SSCALED,
    BUF_FORMAT_8_UINT,
    BUF_FORMAT_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_16
    BUF_FORMAT_16_UNORM,
    BUF_FORMAT_16_SNORM,
    BUF_FORMAT_16_USCALED,
    BUF_FORMAT_16_SSCALED,
    BUF_FORMAT_16_UINT,
    BUF_FORMAT_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_FLOAT,

    //BUF_DATA_FORMAT_8_8
    BUF_FORMAT_8_8_UNORM,
    BUF_FORMAT_8_8_SNORM,
    BUF_FORMAT_8_8_USCALED,
    BUF_FORMAT_8_8_SSCALED,
    BUF_FORMAT_8_8_UINT,
    BUF_FORMAT_8_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_UINT,
    BUF_FORMAT_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_FLOAT,

    //BUF_DATA_FORMAT_16_16
    BUF_FORMAT_16_16_UNORM,
    BUF_FORMAT_16_16_SNORM,
    BUF_FORMAT_16_16_USCALED,
    BUF_FORMAT_16_16_SSCALED,
    BUF_FORMAT_16_16_UINT,
    BUF_FORMAT_16_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_16_FLOAT,

    //BUF_DATA_FORMAT_10_11_11
    BUF_FORMAT_10_11_11_UNORM,
    BUF_FORMAT_10_11_11_SNORM,
    BUF_FORMAT_10_11_11_USCALED,
    BUF_FORMAT_10_11_11_SSCALED,
    BUF_FORMAT_10_11_11_UINT,
    BUF_FORMAT_10_11_11_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_10_11_11_FLOAT,

    //BUF_DATA_FORMAT_11_11_10
    BUF_FORMAT_11_11_10_UNORM,
    BUF_FORMAT_11_11_10_SNORM,
    BUF_FORMAT_11_11_10_USCALED,
    BUF_FORMAT_11_11_10_SSCALED,
    BUF_FORMAT_11_11_10_UINT,
    BUF_FORMAT_11_11_10_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_11_11_10_FLOAT,

    //BUF_DATA_FORMAT_10_10_10_2
    BUF_FORMAT_10_10_10_2_UNORM,
    BUF_FORMAT_10_10_10_2_SNORM,
    BUF_FORMAT_10_10_10_2_USCALED,
    BUF_FORMAT_10_10_10_2_SSCALED,
    BUF_FORMAT_10_10_10_2_UINT,
    BUF_FORMAT_10_10_10_2_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_2_10_10_10
    BUF_FORMAT_2_10_10_10_UNORM,
    BUF_FORMAT_2_10_10_10_SNORM,
    BUF_FORMAT_2_10_10_10_USCALED,
    BUF_FORMAT_2_10_10_10_SSCALED,
    BUF_FORMAT_2_10_10_10_UINT,
    BUF_FORMAT_2_10_10_10_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_8_8_8_8
    BUF_FORMAT_8_8_8_8_UNORM,
    BUF_FORMAT_8_8_8_8_SNORM,
    BUF_FORMAT_8_8_8_8_USCALED,
    BUF_FORMAT_8_8_8_8_SSCALED,
    BUF_FORMAT_8_8_8_8_UINT,
    BUF_FORMAT_8_8_8_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_UINT,
    BUF_FORMAT_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_FLOAT,

    //BUF_DATA_FORMAT_16_16_16_16
    BUF_FORMAT_16_16_16_16_UNORM,
    BUF_FORMAT_16_16_16_16_SNORM,
    BUF_FORMAT_16_16_16_16_USCALED,
    BUF_FORMAT_16_16_16_16_SSCALED,
    BUF_FORMAT_16_16_16_16_UINT,
    BUF_FORMAT_16_16_16_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_16_16_16_FLOAT,

    //BUF_DATA_FORMAT_32_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_UINT,
    BUF_FORMAT_32_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_FLOAT,

    //BUF_DATA_FORMAT_32_32_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_32_UINT,
    BUF_FORMAT_32_32_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_32_FLOAT,

    //BUF_DATA_FORMAT_RESERVED_15
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
};
#endif

// =====================================================================================================================
VertexFetch::VertexFetch(
    Function*           pEntryPoint,      // [in] Entry-point of API vertex shader
    ShaderSystemValues* pShaderSysValues, // [in] ShaderSystemValues object for getting vertex buffer pointer from
    PipelineState*      pPipelineState)   // [in] Pipeline state
    :
    m_pModule(pEntryPoint->getParent()),
    m_pContext(&m_pModule->getContext()),
    m_pShaderSysValues(pShaderSysValues),
    m_pPipelineState(pPipelineState)
{
    LLPC_ASSERT(GetShaderStageFromFunction(pEntryPoint) == ShaderStageVertex); // Must be vertex shader

    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;
    auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
    auto pInsertPos = pEntryPoint->begin()->getFirstInsertionPt();

    // VertexIndex = BaseVertex + VertexID
    if (builtInUsage.vertexIndex)
    {
        auto pBaseVertex = GetFunctionArgument(pEntryPoint, entryArgIdxs.baseVertex);
        auto pVertexId   = GetFunctionArgument(pEntryPoint, entryArgIdxs.vertexId);
        m_pVertexIndex = BinaryOperator::CreateAdd(pBaseVertex, pVertexId, "", &*pInsertPos);
    }

    // InstanceIndex = BaseInstance + InstanceID
    if (builtInUsage.instanceIndex)
    {
        m_pBaseInstance = GetFunctionArgument(pEntryPoint, entryArgIdxs.baseInstance);
        m_pInstanceId   = GetFunctionArgument(pEntryPoint, entryArgIdxs.instanceId);
        m_pInstanceIndex = BinaryOperator::CreateAdd(m_pBaseInstance, m_pInstanceId, "", &*pInsertPos);
    }

    // Initialize default fetch values
    auto pZero = ConstantInt::get(Type::getInt32Ty(*m_pContext), 0);
    auto pOne = ConstantInt::get(Type::getInt32Ty(*m_pContext), 1);

    // Int8 (0, 0, 0, 1)
    m_fetchDefaults.pInt8 = ConstantVector::get({ pZero, pZero, pZero, pOne });

    // Int16 (0, 0, 0, 1)
    m_fetchDefaults.pInt16 = ConstantVector::get({ pZero, pZero, pZero, pOne });

    // Int (0, 0, 0, 1)
    m_fetchDefaults.pInt = ConstantVector::get({ pZero, pZero, pZero, pOne });

    // Int64 (0, 0, 0, 1)
    m_fetchDefaults.pInt64 = ConstantVector::get({ pZero, pZero, pZero, pZero, pZero, pZero, pZero, pOne });

    // Float16 (0, 0, 0, 1.0)
    const uint16_t float16One = 0x3C00;
    auto pFloat16One = ConstantInt::get(Type::getInt32Ty(*m_pContext), float16One);
    m_fetchDefaults.pFloat16 = ConstantVector::get({ pZero, pZero, pZero, pFloat16One });

    // Float (0.0, 0.0, 0.0, 1.0)
    union
    {
        float    f;
        uint32_t u32;
    } floatOne = { 1.0f };
    auto pFloatOne = ConstantInt::get(Type::getInt32Ty(*m_pContext), floatOne.u32);
    m_fetchDefaults.pFloat = ConstantVector::get({ pZero, pZero, pZero, pFloatOne });

    // Double (0.0, 0.0, 0.0, 1.0)
    union
    {
        double   d;
        uint32_t u32[2];
    } doubleOne = { 1.0 };
    auto pDoubleOne0 = ConstantInt::get(Type::getInt32Ty(*m_pContext), doubleOne.u32[0]);
    auto pDoubleOne1 = ConstantInt::get(Type::getInt32Ty(*m_pContext), doubleOne.u32[1]);
    m_fetchDefaults.pDouble = ConstantVector::get({ pZero, pZero,
                                                    pZero, pZero,
                                                    pZero, pZero,
                                                    pDoubleOne0, pDoubleOne1 });
}

// =====================================================================================================================
// Executes vertex fetch operations based on the specified vertex input type and its location.
Value* VertexFetch::Run(
    Type*        pInputTy,      // [in] Type of vertex input
    uint32_t     location,      // Location of vertex input
    uint32_t     compIdx,       // Index used for vector element indexing
    Instruction* pInsertPos)    // [in] Where to insert vertex fetch instructions
{
    Value* pVertex = nullptr;

    // Get vertex input description for the given location
    const VertexInputDescription* pDescription = m_pPipelineState->FindVertexInputDescription(location);

    // NOTE: If we could not find vertex input info matching this location, just return undefined value.
    if (pDescription == nullptr)
    {
        return UndefValue::get(pInputTy);
    }

    auto pVbDesc = LoadVertexBufferDescriptor(pDescription->binding, pInsertPos);

    Value* pVbIndex = nullptr;
    if (pDescription->inputRate == VertexInputRateVertex)
    {
        pVbIndex = GetVertexIndex(); // Use vertex index
    }
    else
    {
        if (pDescription->inputRate == VertexInputRateNone)
        {
            pVbIndex = m_pBaseInstance;
        }
        else if (pDescription->inputRate == VertexInputRateInstance)
        {
            pVbIndex = GetInstanceIndex(); // Use instance index
        }
        else
        {
            // There is a divisor.
            pVbIndex = BinaryOperator::CreateUDiv(m_pInstanceId,
                                                  ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                                                   pDescription->inputRate),
                                                  "",
                                                  pInsertPos);
            pVbIndex = BinaryOperator::CreateAdd(pVbIndex, m_pBaseInstance, "", pInsertPos);
        }
    }

    Value* vertexFetch[2] = {}; // Two vertex fetch operations might be required
    Value* pVertexFetch = nullptr; // Coalesced vector by combining the results of two vertex fetch operations

    VertexFormatInfo formatInfo = GetVertexFormatInfo(pDescription);

    const bool is8bitFetch = (pInputTy->getScalarSizeInBits() == 8);
    const bool is16bitFetch = (pInputTy->getScalarSizeInBits() == 16);

    // Do the first vertex fetch operation
    AddVertexFetchInst(pVbDesc,
                       formatInfo.numChannels,
                       is16bitFetch,
                       pVbIndex,
                       pDescription->offset,
                       pDescription->stride,
                       formatInfo.dfmt,
                       formatInfo.nfmt,
                       pInsertPos,
                       &vertexFetch[0]);

    // Do post-processing in certain cases
    std::vector<Constant*> shuffleMask;
    bool postShuffle = NeedPostShuffle(pDescription, shuffleMask);
    bool patchA2S = NeedPatchA2S(pDescription);
    if (postShuffle || patchA2S)
    {
        if (postShuffle)
        {
            // NOTE: If we are fetching a swizzled format, we have to add an extra "shufflevector" instruction to
            // get the components in the right order.
            LLPC_ASSERT(shuffleMask.empty() == false);
            vertexFetch[0] = new ShuffleVectorInst(vertexFetch[0],
                                                   vertexFetch[0],
                                                   ConstantVector::get(shuffleMask),
                                                   "",
                                                   pInsertPos);
        }

        if (patchA2S)
        {
            LLPC_ASSERT(vertexFetch[0]->getType()->getVectorNumElements() == 4);

            // Extract alpha channel: %a = extractelement %vf0, 3
            Value* pAlpha = ExtractElementInst::Create(vertexFetch[0],
                                                       ConstantInt::get(Type::getInt32Ty(*m_pContext), 3),
                                                       "",
                                                       pInsertPos);

            if (formatInfo.nfmt == BufNumFormatSint)
            {
                // NOTE: For format "SINT 10_10_10_2", vertex fetches incorrectly return the alpha channel as
                // unsigned. We have to manually sign-extend it here by doing a "shl" 30 then an "ashr" 30.

                // %a = shl %a, 30
                pAlpha = BinaryOperator::CreateShl(pAlpha,
                                                   ConstantInt::get(Type::getInt32Ty(*m_pContext), 30),
                                                   "",
                                                   pInsertPos);

                // %a = ashr %a, 30
                pAlpha = BinaryOperator::CreateAShr(pAlpha,
                                                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 30),
                                                    "",
                                                    pInsertPos);
            }
            else if (formatInfo.nfmt == BufNumFormatSnorm)
            {
                // NOTE: For format "SNORM 10_10_10_2", vertex fetches incorrectly return the alpha channel
                // as unsigned. We have to somehow remap the values { 0.0, 0.33, 0.66, 1.00 } to { 0.0, 1.0,
                // -1.0, -1.0 } respectively.

                // %a = bitcast %a to f32
                pAlpha = new BitCastInst(pAlpha, Type::getFloatTy(*m_pContext), "", pInsertPos);

                // %a = mul %a, 3.0f
                pAlpha = BinaryOperator::CreateFMul(pAlpha,
                                                   ConstantFP::get(Type::getFloatTy(*m_pContext), 3.0f),
                                                   "",
                                                   pInsertPos);

                // %cond = ugt %a, 1.5f
                auto pCond = new FCmpInst(pInsertPos,
                                          FCmpInst::FCMP_UGT,
                                          pAlpha,
                                          ConstantFP::get(Type::getFloatTy(*m_pContext), 1.5f),
                                          "");

                // %a = select %cond, -1.0f, pAlpha
                pAlpha = SelectInst::Create(pCond,
                                            ConstantFP::get(Type::getFloatTy(*m_pContext), -1.0f),
                                            pAlpha,
                                            "",
                                            pInsertPos);

                // %a = bitcast %a to i32
                pAlpha = new BitCastInst(pAlpha, Type::getInt32Ty(*m_pContext), "", pInsertPos);
            }
            else if (formatInfo.nfmt == BufNumFormatSscaled)
            {
                // NOTE: For format "SSCALED 10_10_10_2", vertex fetches incorrectly return the alpha channel
                // as unsigned. We have to somehow remap the values { 0.0, 1.0, 2.0, 3.0 } to { 0.0, 1.0,
                // -2.0, -1.0 } respectively. We can perform the sign extension here by doing a "fptosi", "shl" 30,
                // "ashr" 30, and finally "sitofp".

               // %a = bitcast %a to float
                pAlpha = new BitCastInst(pAlpha, Type::getFloatTy(*m_pContext), "", pInsertPos);

                // %a = fptosi %a to i32
                pAlpha = new FPToSIInst(pAlpha, Type::getInt32Ty(*m_pContext), "", pInsertPos);

                // %a = shl %a, 30
                pAlpha = BinaryOperator::CreateShl(pAlpha,
                                                   ConstantInt::get(Type::getInt32Ty(*m_pContext), 30),
                                                   "",
                                                   pInsertPos);

                // %a = ashr a, 30
                pAlpha = BinaryOperator::CreateAShr(pAlpha,
                                                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 30),
                                                    "",
                                                    pInsertPos);

                // %a = sitofp %a to float
                pAlpha = new SIToFPInst(pAlpha, Type::getFloatTy(*m_pContext), "", pInsertPos);

                // %a = bitcast %a to i32
                pAlpha = new BitCastInst(pAlpha, Type::getInt32Ty(*m_pContext), "", pInsertPos);
            }
            else
            {
                LLPC_NEVER_CALLED();
            }

            // Insert alpha channel: %vf0 = insertelement %vf0, %a, 3
            vertexFetch[0] = InsertElementInst::Create(vertexFetch[0],
                                                       pAlpha,
                                                       ConstantInt::get(Type::getInt32Ty(*m_pContext), 3),
                                                       "",
                                                       pInsertPos);
        }
    }

    // Do the second vertex fetch operation
    const bool secondFetch = NeedSecondVertexFetch(pDescription);
    if (secondFetch)
    {
        uint32_t numChannels = formatInfo.numChannels;
        uint32_t dfmt = formatInfo.dfmt;

        if (pDescription->dfmt == BufDataFormat64_64_64)
        {
            // Valid number of channels and data format have to be revised
            numChannels = 2;
            dfmt = BUF_DATA_FORMAT_32_32;
        }

        AddVertexFetchInst(pVbDesc,
                           numChannels,
                           is16bitFetch,
                           pVbIndex,
                           pDescription->offset + SizeOfVec4,
                           pDescription->stride,
                           dfmt,
                           formatInfo.nfmt,
                           pInsertPos,
                           &vertexFetch[1]);
    }

    if (secondFetch)
    {
        // NOTE: If we performs vertex fetch operations twice, we have to coalesce result values of the two
        // fetch operations and generate a combined one.
        LLPC_ASSERT((vertexFetch[0] != nullptr) && (vertexFetch[1] != nullptr));
        LLPC_ASSERT(vertexFetch[0]->getType()->getVectorNumElements() == 4);

        uint32_t compCount = vertexFetch[1]->getType()->getVectorNumElements();
        LLPC_ASSERT((compCount == 2) || (compCount == 4)); // Should be <2 x i32> or <4 x i32>

        if (compCount == 2)
        {
            // NOTE: We have to enlarge the second vertex fetch, from <2 x i32> to <4 x i32>. Otherwise,
            // vector shuffle operation could not be performed in that it requires the two vectors have
            // the same types.

            // %vf1 = shufflevector %vf1, %vf1, <0, 1, undef, undef>
            Constant* shuffleMask[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                UndefValue::get(Type::getInt32Ty(*m_pContext)),
                UndefValue::get(Type::getInt32Ty(*m_pContext))
            };
            vertexFetch[1] = new ShuffleVectorInst(vertexFetch[1],
                                                   vertexFetch[1],
                                                   ConstantVector::get(shuffleMask),
                                                   "",
                                                   pInsertPos);
        }

        // %vf = shufflevector %vf0, %vf1, <0, 1, 2, 3, 4, 5, ...>
        shuffleMask.clear();
        for (uint32_t i = 0; i < 4 + compCount; ++i)
        {
            shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), i));
        }
        pVertexFetch = new ShuffleVectorInst(vertexFetch[0],
                                             vertexFetch[1],
                                             ConstantVector::get(shuffleMask),
                                             "",
                                             pInsertPos);
    }
    else
    {
        pVertexFetch = vertexFetch[0];
    }

    // Finalize vertex fetch
    Type* pBasicTy = pInputTy->isVectorTy() ? pInputTy->getVectorElementType() : pInputTy;
    const uint32_t bitWidth = pBasicTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));

    // Get default fetch values
    Constant* pDefaults = nullptr;

    if (pBasicTy->isIntegerTy())
    {
        if (bitWidth == 8)
        {
            pDefaults = m_fetchDefaults.pInt8;
        }
        else if (bitWidth == 16)
        {
            pDefaults = m_fetchDefaults.pInt16;
        }
        else if (bitWidth == 32)
        {
            pDefaults = m_fetchDefaults.pInt;
        }
        else
        {
            LLPC_ASSERT(bitWidth == 64);
            pDefaults = m_fetchDefaults.pInt64;
        }
    }
    else if (pBasicTy->isFloatingPointTy())
    {
        if (bitWidth == 16)
        {
            pDefaults = m_fetchDefaults.pFloat16;
        }
        else if (bitWidth == 32)
        {
            pDefaults = m_fetchDefaults.pFloat;
        }
        else
        {
            LLPC_ASSERT(bitWidth == 64);
            pDefaults = m_fetchDefaults.pDouble;
        }
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    const uint32_t defaultCompCount = pDefaults->getType()->getVectorNumElements();
    std::vector<Value*> defaultValues(defaultCompCount);

    for (uint32_t i = 0; i < defaultValues.size(); ++i)
    {
        defaultValues[i] = ExtractElementInst::Create(pDefaults,
                                                      ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                      "",
                                                      pInsertPos);
    }

    // Get vertex fetch values
    const uint32_t fetchCompCount = pVertexFetch->getType()->isVectorTy() ?
                                        pVertexFetch->getType()->getVectorNumElements() : 1;
    std::vector<Value*> fetchValues(fetchCompCount);

    if (fetchCompCount == 1)
    {
        fetchValues[0] = pVertexFetch;
    }
    else
    {
        for (uint32_t i = 0; i < fetchCompCount; ++i)
        {
            fetchValues[i] = ExtractElementInst::Create(pVertexFetch,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                        "",
                                                        pInsertPos);
        }
    }

    // Construct vertex fetch results
    const uint32_t inputCompCount = pInputTy->isVectorTy() ? pInputTy->getVectorNumElements() : 1;
    const uint32_t vertexCompCount = inputCompCount * ((bitWidth == 64) ? 2 : 1);

    std::vector<Value*> vertexValues(vertexCompCount);

    // NOTE: Original component index is based on the basic scalar type.
    compIdx *= ((bitWidth == 64) ? 2 : 1);

    // Vertex input might take values from vertex fetch values or default fetch values
    for (uint32_t i = 0; i < vertexCompCount; i++)
    {
        if (compIdx + i < fetchCompCount)
        {
            vertexValues[i] = fetchValues[compIdx + i];
        }
        else if (compIdx + i < defaultCompCount)
        {
            vertexValues[i] = defaultValues[compIdx + i];
        }
        else
        {
            LLPC_NEVER_CALLED();
            vertexValues[i] = UndefValue::get(Type::getInt32Ty(*m_pContext));
        }
    }

    if (vertexCompCount == 1)
    {
        pVertex = vertexValues[0];
    }
    else
    {
        Type* pVertexTy = VectorType::get(Type::getInt32Ty(*m_pContext), vertexCompCount);
        pVertex = UndefValue::get(pVertexTy);

        for (uint32_t i = 0; i < vertexCompCount; ++i)
        {
            pVertex = InsertElementInst::Create(pVertex,
                                                vertexValues[i],
                                                ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                "",
                                                pInsertPos);
        }
    }

    if (is8bitFetch)
    {
        // NOTE: The vertex fetch results are represented as <n x i32> now. For 8-bit vertex fetch, we have to
        // convert them to <n x i8> and the 24 high bits is truncated.
        LLPC_ASSERT(pInputTy->isIntOrIntVectorTy()); // Must be integer type

        Type* pVertexTy = pVertex->getType();
        Type* pTruncTy = Type::getInt8Ty(*m_pContext);
        pTruncTy = pVertexTy->isVectorTy() ? cast<Type>(VectorType::get(pTruncTy, pVertexTy->getVectorNumElements())) :
                                             pTruncTy;
        pVertex = new TruncInst(pVertex, pTruncTy, "", pInsertPos);
    }
    else if (is16bitFetch)
    {
        // NOTE: The vertex fetch results are represented as <n x i32> now. For 16-bit vertex fetch, we have to
        // convert them to <n x i16> and the 16 high bits is truncated.
        Type* pVertexTy = pVertex->getType();
        Type* pTruncTy = Type::getInt16Ty(*m_pContext);
        pTruncTy = pVertexTy->isVectorTy() ? cast<Type>(VectorType::get(pTruncTy, pVertexTy->getVectorNumElements())) :
                                             pTruncTy;
        pVertex = new TruncInst(pVertex, pTruncTy, "", pInsertPos);
    }

    return pVertex;
}

// =====================================================================================================================
// Gets info from table according to vertex attribute format.
VertexFormatInfo VertexFetch::GetVertexFormatInfo(
    const VertexInputDescription* pInputDesc)    // [in] Vertex input description
{
    VertexFormatInfo info = {
                                static_cast<BufNumFormat>(pInputDesc->nfmt),
                                static_cast<BufDataFormat>(pInputDesc->dfmt),
                                1
                            };
    switch (pInputDesc->dfmt)
    {
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
const VertexCompFormatInfo* VertexFetch::GetVertexComponentFormatInfo(
    uint32_t dfmt) // Date format of vertex buffer
{
    LLPC_ASSERT(dfmt < sizeof(m_vertexCompFormatInfo) / sizeof(m_vertexCompFormatInfo[0]));
    return &m_vertexCompFormatInfo[dfmt];
}

// =====================================================================================================================
// Maps separate buffer data and numeric formats to the combined buffer format
uint32_t VertexFetch::MapVertexFormat(
    uint32_t dfmt,  // Data format
    uint32_t nfmt   // Numeric format
    ) const
{
    LLPC_ASSERT(dfmt < 16);
    LLPC_ASSERT(nfmt < 8);
    uint32_t format = 0;

#if LLPC_BUILD_GFX10
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    if (gfxIp.major >= 10)
    {
        uint32_t index = (dfmt * 8) + nfmt;
        LLPC_ASSERT(index < sizeof(m_vertexFormatMap) / sizeof(m_vertexFormatMap[0]));
        format = m_vertexFormatMap[index];
    }
    else
#endif
    {
        CombineFormat formatOprd = {};
        formatOprd.bits.dfmt = dfmt;
        formatOprd.bits.nfmt = nfmt;
        format = formatOprd.u32All;
    }
    return format;
}

// =====================================================================================================================
// Loads vertex descriptor based on the specified vertex input location.
Value* VertexFetch::LoadVertexBufferDescriptor(
    uint32_t     binding,       // ID of vertex buffer binding
    Instruction* pInsertPos     // [in] Where to insert instructions
    ) const
{
    Value* idxs[] = {
        ConstantInt::get(Type::getInt64Ty(*m_pContext), 0, false),
        ConstantInt::get(Type::getInt64Ty(*m_pContext), binding, false)
    };

    auto pVbTablePtr = m_pShaderSysValues->GetVertexBufTablePtr();
    auto pVbDescPtr = GetElementPtrInst::Create(nullptr, pVbTablePtr, idxs, "", pInsertPos);
    pVbDescPtr->setMetadata(MetaNameUniform, MDNode::get(pVbDescPtr->getContext(), {}));

    auto pVbDesc = new LoadInst(pVbDescPtr, "", pInsertPos);
    pVbDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(pVbDesc->getContext(), {}));
    pVbDesc->setAlignment(MaybeAlign(16));

    return pVbDesc;
}

// =====================================================================================================================
// Inserts instructions to do vertex fetch operations.
void VertexFetch::AddVertexFetchInst(
    Value*       pVbDesc,       // [in] Vertex buffer descriptor
    uint32_t     numChannels,   // Valid number of channels
    bool         is16bitFetch,  // Whether it is 16-bit vertex fetch
    Value*       pVbIndex,      // [in] Index of vertex fetch in buffer
    uint32_t     offset,        // Vertex attribute offset (in bytes)
    uint32_t     stride,        // Vertex attribute stride (in bytes)
    uint32_t     dfmt,          // Date format of vertex buffer
    uint32_t     nfmt,          // Numeric format of vertex buffer
    Instruction* pInsertPos,    // [in] Where to insert instructions
    Value**      ppFetch        // [out] Destination of vertex fetch
    ) const
{
    const VertexCompFormatInfo* pFormatInfo = GetVertexComponentFormatInfo(dfmt);

    // NOTE: If the vertex attribute offset and stride are aligned on data format boundaries, we can do a vertex fetch
    // operation to read the whole vertex. Otherwise, we have to do vertex per-component fetch operations.
    if ((((offset % pFormatInfo->vertexByteSize) == 0) && ((stride % pFormatInfo->vertexByteSize) == 0)) ||
        (pFormatInfo->compDfmt == dfmt))
    {
        // NOTE: If the vertex attribute offset is greater than vertex attribute stride, we have to adjust both vertex
        // buffer index and vertex attribute offset accordingly. Otherwise, vertex fetch might behave unexpectedly.
        if ((stride != 0) && (offset > stride))
        {
            pVbIndex = BinaryOperator::CreateAdd(pVbIndex,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), offset / stride),
                                                 "",
                                                 pInsertPos);
            offset = offset % stride;
        }

        // Do vertex fetch
        Value* args[] = {
            pVbDesc,                                                                        // rsrc
            pVbIndex,                                                                       // vindex
            ConstantInt::get(Type::getInt32Ty(*m_pContext), offset),                        // offset
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),                             // soffset
            ConstantInt::get(Type::getInt32Ty(*m_pContext), MapVertexFormat(dfmt, nfmt)),   // dfmt, nfmt
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 0)                              // glc, slc
        };

        StringRef suffix = "";
        Type* pFetchTy = nullptr;

        if (is16bitFetch)
        {
            switch (numChannels)
            {
            case 1:
                suffix = ".f16";
                pFetchTy = Type::getHalfTy(*m_pContext);
                break;
            case 2:
                suffix = ".v2f16";
                pFetchTy = VectorType::get(Type::getHalfTy(*m_pContext), 2);
                break;
            case 3:
            case 4:
                suffix = ".v4f16";
                pFetchTy = VectorType::get(Type::getHalfTy(*m_pContext), 4);
                break;
            default:
                LLPC_NEVER_CALLED();
                break;
            }
        }
        else
        {
            switch (numChannels)
            {
            case 1:
                suffix = ".i32";
                pFetchTy = Type::getInt32Ty(*m_pContext);
                break;
            case 2:
                suffix = ".v2i32";
                pFetchTy = VectorType::get(Type::getInt32Ty(*m_pContext), 2);
                break;
            case 3:
            case 4:
                suffix = ".v4i32";
                pFetchTy = VectorType::get(Type::getInt32Ty(*m_pContext), 4);
                break;
            default:
                LLPC_NEVER_CALLED();
                break;
            }
        }

        Value* pFetch = EmitCall((Twine("llvm.amdgcn.struct.tbuffer.load") + suffix).str(),
                                 pFetchTy,
                                 args,
                                 NoAttrib,
                                 pInsertPos);

        if (is16bitFetch)
        {
            // NOTE: The fetch values are represented by <n x i32>, so we will bitcast the float16 values to
            // int32 eventually.
            Type* pBitCastTy = Type::getInt16Ty(*m_pContext);
            pBitCastTy = (numChannels == 1) ? pBitCastTy : VectorType::get(pBitCastTy, numChannels);
            pFetch = new BitCastInst(pFetch,
                                        pBitCastTy,
                                        "",
                                        pInsertPos);

            Type* pZExtTy = Type::getInt32Ty(*m_pContext);
            pZExtTy = (numChannels == 1) ? pZExtTy : VectorType::get(pZExtTy, numChannels);
            pFetch = new ZExtInst(pFetch,
                                    pZExtTy,
                                    "",
                                    pInsertPos);
        }

        if (numChannels == 3)
        {
            // NOTE: If valid number of channels is 3, the actual fetch type should be revised from <4 x i32>
            // to <3 x i32>.
            Constant* shuffleMask[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 2)
            };
            *ppFetch = new ShuffleVectorInst(pFetch, pFetch, ConstantVector::get(shuffleMask), "", pInsertPos);
        }
        else
        {
            *ppFetch = pFetch;
        }
    }
    else
    {
        // NOTE: Here, we split the vertex into its components and do per-component fetches. The expectation
        // is that the vertex per-component fetches always match the hardware requirements.
        LLPC_ASSERT(numChannels == pFormatInfo->compCount);

        Value* compVbIndices[4]  = {};
        uint32_t compOffsets[4] = {};

        for (uint32_t i = 0; i < pFormatInfo->compCount; ++i)
        {
            uint32_t compOffset = offset + i * pFormatInfo->compByteSize;

            // NOTE: If the vertex attribute per-component offset is greater than vertex attribute stride, we have
            // to adjust both vertex buffer index and vertex per-component offset accordingly. Otherwise, vertex
            // fetch might behave unexpectedly.
            if ((stride != 0) && (compOffset > stride))
            {
                compVbIndices[i] = BinaryOperator::CreateAdd(
                                       pVbIndex,
                                       ConstantInt::get(Type::getInt32Ty(*m_pContext), compOffset / stride),
                                       "",
                                       pInsertPos);
                compOffsets[i] = compOffset % stride;
            }
            else
            {
                compVbIndices[i] = pVbIndex;
                compOffsets[i] = compOffset;
            }
        }

        Type* pFetchTy = VectorType::get(Type::getInt32Ty(*m_pContext), numChannels);
        Value* pFetch = UndefValue::get(pFetchTy);

        // Do vertex per-component fetches
        for (uint32_t i = 0; i < pFormatInfo->compCount; ++i)
        {
            Value* args[] = {
                pVbDesc,                                                          // rsrc
                compVbIndices[i],                                                 // vindex
                ConstantInt::get(Type::getInt32Ty(*m_pContext), compOffsets[i]),  // offset
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),               // soffset
                ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                 MapVertexFormat(pFormatInfo->compDfmt, nfmt)),   // dfmt, nfmt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0)                // glc, slc
            };

            Value* pCompFetch = nullptr;
            if (is16bitFetch)
            {
                pCompFetch = EmitCall("llvm.amdgcn.struct.tbuffer.load.f16",
                                      Type::getHalfTy(*m_pContext),
                                      args,
                                      NoAttrib,
                                      pInsertPos);

                pCompFetch = new BitCastInst(pCompFetch, Type::getInt16Ty(*m_pContext), "", pInsertPos);
                pCompFetch = new ZExtInst(pCompFetch, Type::getInt32Ty(*m_pContext), "", pInsertPos);
            }
            else
            {
                pCompFetch = EmitCall("llvm.amdgcn.struct.tbuffer.load.i32",
                                      Type::getInt32Ty(*m_pContext),
                                      args,
                                      NoAttrib,
                                      pInsertPos);
            }

            pFetch = InsertElementInst::Create(pFetch,
                                               pCompFetch,
                                               ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                               "",
                                               pInsertPos);
        }

        *ppFetch = pFetch;
    }
}

// =====================================================================================================================
// Checks whether post shuffle is required for vertex fetch oepration.
bool VertexFetch::NeedPostShuffle(
    const VertexInputDescription*  pInputDesc,   // [in] Vertex input description
    std::vector<Constant*>&        shuffleMask   // [out] Vector shuffle mask
    ) const
{
    bool needShuffle = false;

    switch (pInputDesc->dfmt)
    {
    case BufDataFormat8_8_8_8_Bgra:
    case BufDataFormat2_10_10_10_Bgra:
        shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), 2));
        shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), 1));
        shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), 0));
        shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), 3));
        needShuffle = true;
        break;
    default:
        break;
    }

    return needShuffle;
}

// =====================================================================================================================
// Checks whether patching 2-bit signed alpha channel is required for vertex fetch operation.
bool VertexFetch::NeedPatchA2S(
    const VertexInputDescription* pInputDesc    // [in] Vertex input description
    ) const
{
    bool needPatch = false;

    if ((pInputDesc->dfmt == BufDataFormat2_10_10_10) ||
        (pInputDesc->dfmt == BufDataFormat2_10_10_10_Bgra))
    {
        if ((pInputDesc->nfmt == BufNumFormatSnorm) ||
            (pInputDesc->nfmt == BufNumFormatSscaled) ||
            (pInputDesc->nfmt == BufNumFormatSint))
        {
            needPatch = (m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major < 9);
        }
    }

    return needPatch;
}

// =====================================================================================================================
// Checks whether the second vertex fetch operation is required (particularly for certain 64-bit typed formats).
bool VertexFetch::NeedSecondVertexFetch(
    const VertexInputDescription* pInputDesc    // [in] Vertex input description
    ) const
{
    return ((pInputDesc->dfmt == BufDataFormat64_64_64) ||
            (pInputDesc->dfmt == BufDataFormat64_64_64_64));
}

} // Llpc
