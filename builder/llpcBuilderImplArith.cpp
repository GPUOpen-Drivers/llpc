/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderImplArith.cpp
 * @brief LLPC source file: implementation of arithmetic Builder methods
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcContext.h"

#define DEBUG_TYPE "llpc-builder-impl-arith"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
// the given cube map texture coordinates. Returns <2 x float>.
Value* BuilderImplArith::CreateCubeFaceCoord(
    Value*        pCoord,     // [in] Input coordinate <3 x float>
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pCoordX = CreateExtractElement(pCoord, uint64_t(0));
    Value* pCoordY = CreateExtractElement(pCoord, 1);
    Value* pCoordZ = CreateExtractElement(pCoord, 2);
    Value* pCubeMa = CreateIntrinsic(Intrinsic::amdgcn_cubema, {}, { pCoordX, pCoordY, pCoordZ }, nullptr);
    Value* pRecipMa = CreateFDiv(ConstantFP::get(getFloatTy(), 1.0), pCubeMa);
    Value* pCubeSc = CreateIntrinsic(Intrinsic::amdgcn_cubesc, {}, { pCoordX, pCoordY, pCoordZ }, nullptr);
    Value* pScDivMa = CreateFMul(pRecipMa, pCubeSc);
    Value* pResultX = CreateFAdd(pScDivMa, ConstantFP::get(getFloatTy(), 0.5));
    Value* pCubeTc = CreateIntrinsic(Intrinsic::amdgcn_cubetc, {}, { pCoordX, pCoordY, pCoordZ }, nullptr);
    Value* pTcDivMa = CreateFMul(pRecipMa, pCubeTc);
    Value* pResultY = CreateFAdd(pTcDivMa, ConstantFP::get(getFloatTy(), 0.5));
    Value* pResult = CreateInsertElement(UndefValue::get(VectorType::get(getFloatTy(), 2)), pResultX, uint64_t(0));
    pResult = CreateInsertElement(pResult, pResultY, 1, instName);
    return pResult;
}

// =====================================================================================================================
// Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
// the given cube map texture coordinates. Returns a single float with value:
//  0.0 = the cube map face facing the positive X direction
//  1.0 = the cube map face facing the negative X direction
//  2.0 = the cube map face facing the positive Y direction
//  3.0 = the cube map face facing the negative Y direction
//  4.0 = the cube map face facing the positive Z direction
//  5.0 = the cube map face facing the negative Z direction
Value* BuilderImplArith::CreateCubeFaceIndex(
    Value*        pCoord,     // [in] Input coordinate <3 x float>
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pCoordX = CreateExtractElement(pCoord, uint64_t(0));
    Value* pCoordY = CreateExtractElement(pCoord, 1);
    Value* pCoordZ = CreateExtractElement(pCoord, 2);
    return CreateIntrinsic(Intrinsic::amdgcn_cubeid, {}, { pCoordX, pCoordY, pCoordZ }, nullptr, instName);
}

// =====================================================================================================================
// Create quantize operation: truncates float (or vector) value to a value that is representable by a half.
Value* BuilderImplArith::CreateQuantizeToFp16(
    Value*        pValue,     // [in] Input value (float or float vector)
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    LLPC_ASSERT(pValue->getType()->getScalarType()->isFloatTy());

    Constant* pZero = Constant::getNullValue(pValue->getType());
    // 2^-15 (normalized float16 minimum)
    Constant* pMinNormalizedHalf = ConstantFP::get(pValue->getType(), 1.0 / 32768.0);

    Value* pTrunc = CreateFPTrunc(pValue, GetConditionallyVectorizedTy(getHalfTy(), pValue->getType()));
    Value* pExt = CreateFPExt(pTrunc, pValue->getType());
    Value* pAbs = CreateIntrinsic(Intrinsic::fabs, pExt->getType(), pExt);
    Value* pIsLessThanMin = CreateFCmpOLT(pAbs, pMinNormalizedHalf);
    Value* pIsNotZero = CreateFCmpONE(pAbs, pZero);
    Value* pIsDenorm = CreateAnd(pIsLessThanMin, pIsNotZero);
    Value* pResult = CreateSelect(pIsDenorm, pZero, pExt);

    // Check NaN. 
    Value* pIsNan = CreateIsNaN(pValue);
    return CreateSelect(pIsNan, pValue, pResult, instName);
}

// =====================================================================================================================
// Create signed integer modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
Value* BuilderImplArith::CreateSMod(
    Value*        pDividend,  // [in] Dividend value
    Value*        pDivisor,   // [in] Divisor value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
#if LLPC_BUILD_GFX10
    if (pDivisor->getType()->getScalarType()->isIntegerTy(32) &&
        (getContext().GetGpuWorkarounds()->gfx10.disableI32ModToI16Mod))
    {

        // NOTE: On some hardware, when the divisor is a literal value and less than 0xFFFF, i32 mod will be
        // optimized to i16 mod. There is an existing issue in the backend which makes i16 mod not work.
        // This is the workaround to this issue.
        // TODO: Check if this is still needed and what the backend problem is.
        if (auto pDivisorConst = dyn_cast<ConstantInt>(pDivisor))
        {
            if (pDivisorConst->getZExtValue() <= 0xFFFF)
            {
                // Get a non-constant 0 value. (We know the top 17 bits of the 64-bit PC is always zero.)
                Value* pPc = CreateIntrinsic(Intrinsic::amdgcn_s_getpc, getInt64Ty(), {});
                Value* pPcHi = CreateExtractElement(CreateBitCast(pPc, VectorType::get(getInt32Ty(), 2)), 1);
                Value* pNonConstantZero = CreateLShr(pPcHi, getInt32(15));
                if (auto pVecTy = dyn_cast<VectorType>(pDivisor->getType()))
                {
                    pNonConstantZero = CreateVectorSplat(pVecTy->getNumElements(), pNonConstantZero);
                }
                // Add the non-constant 0 to the denominator to disable the optimization.
                pDivisor = CreateAdd(pDivisor, pNonConstantZero);
            }
        }
    }
#endif

    Value* pSrem = CreateSRem(pDividend, pDivisor);
    Value* pDivisorPlusSrem = CreateAdd(pDivisor, pSrem);
    Value* pIsDifferentSign = CreateICmpSLT(CreateXor(pDividend, pDivisor),
                                            Constant::getNullValue(pDividend->getType()));
    Value* pRemainderNotZero = CreateICmpNE(pSrem, Constant::getNullValue(pSrem->getType()));
    Value* pResultNeedsAddDivisor = CreateAnd(pIsDifferentSign, pRemainderNotZero);
    return CreateSelect(pResultNeedsAddDivisor, pDivisorPlusSrem, pSrem, instName);
}

// =====================================================================================================================
// Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
Value* BuilderImplArith::CreateIsNaN(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // 0x001: signaling NaN, 0x002: quiet NaN
    return CreateCallAmdgcnClass(pX, 0x003, instName);
}

// =====================================================================================================================
// Helper method to create call to llvm.amdgcn.class, scalarizing if necessary. This is not exposed outside of
// BuilderImplArith.
Value* BuilderImplArith::CreateCallAmdgcnClass(
    Value*        pValue,     // [in] Input value
    uint32_t      flags,      // Flags for what class(es) to check for
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pResult = Scalarize(pValue,
                               [this, flags](Value* pValue)
                               {
                                  return CreateIntrinsic(Intrinsic::amdgcn_class,
                                                         pValue->getType(),
                                                         { pValue, getInt32(flags) });
                               });
    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector unary operation
Value* BuilderImplArith::Scalarize(
    Value*                        pValue,     // [in] Input value
    std::function<Value*(Value*)> callback)   // [in] Callback function
{
    if (auto pVecTy = dyn_cast<VectorType>(pValue->getType()))
    {
        Value* pResult0 = callback(CreateExtractElement(pValue, uint64_t(0)));
        Value* pResult = UndefValue::get(VectorType::get(pResult0->getType(), pVecTy->getNumElements()));
        pResult = CreateInsertElement(pResult, pResult0, uint64_t(0));
        for (uint32_t idx = 1, end = pVecTy->getNumElements(); idx != end; ++idx)
        {
            pResult = CreateInsertElement(pResult, callback(CreateExtractElement(pValue, idx)), idx);
        }
        return pResult;
    }
    Value* pResult = callback(pValue);
    return pResult;
}

