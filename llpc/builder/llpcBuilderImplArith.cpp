/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/IntrinsicsAMDGPU.h"

#include "llpcBuilderImpl.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"

#define DEBUG_TYPE "llpc-builder-impl-arith"

using namespace lgc;
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
// Create scalar or vector FP truncate operation with the given rounding mode.
// Currently the rounding mode is only implemented for float/double -> half conversion.
Value* BuilderImplArith::CreateFpTruncWithRounding(
    Value*            pValue,             // [in] Input value
    Type*             pDestTy,            // [in] Type to convert to
    unsigned          roundingMode,       // Rounding mode
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    if (pValue->getType()->getScalarType()->isDoubleTy())
    {
        pValue = CreateFPTrunc(pValue, GetConditionallyVectorizedTy(getFloatTy(), pDestTy));
    }

    if (pValue->getType() == pDestTy)
    {
        return pValue;
    }

    assert(pValue->getType()->getScalarType()->isFloatTy() && pDestTy->getScalarType()->isHalfTy());

    // RTZ: Use cvt_pkrtz instruction.
    // TODO: We also use this for RTP and RTN for now.
    // TODO: Using a hard-coded value for rmToNearest due to flux in LLVM over
    // the namespace for this value - this will be removed once it has settled
    //if (roundingMode != fp::rmToNearest)
    if (roundingMode != 1 /* rmToNearest */ )
    {
        Value* pResult = ScalarizeInPairs(pValue,
                                          [this](Value* pInVec2)
                                          {
                                              Value* pInVal0 = CreateExtractElement(pInVec2, uint64_t(0));
                                              Value* pInVal1 = CreateExtractElement(pInVec2, 1);
                                              return CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz,
                                                                     {},
                                                                     { pInVal0, pInVal1 });
                                          });
        pResult->setName(instName);
        return pResult;
    }

    // RTE.
    // float32: sign = [31], exponent = [30:23], mantissa = [22:0]
    // float16: sign = [15], exponent = [14:10], mantissa = [9:0]
    Value* pBits32 = CreateBitCast(pValue, GetConditionallyVectorizedTy(getInt32Ty(), pValue->getType()));

    // sign16 = (bits32 >> 16) & 0x8000
    Value* pSign16 = CreateAnd(CreateLShr(pBits32, ConstantInt::get(pBits32->getType(), 16)),
                               ConstantInt::get(pBits32->getType(), 0x8000));

    // exp32 = (bits32 >> 23) & 0xFF
    Value* pExp32 = CreateAnd(CreateLShr(pBits32, ConstantInt::get(pBits32->getType(), 23)),
                              ConstantInt::get(pBits32->getType(), 0xFF));

    // exp16 = exp32 - 127 + 15
    Value* pExp16 = CreateSub(pExp32, ConstantInt::get(pExp32->getType(), (127 - 15)));

    // mant32 = bits32 & 0x7FFFFF
    Value* pMant32 = CreateAnd(pBits32, 0x7FFFFF);

    Value* pIsNanInf = CreateICmpEQ(pExp32, ConstantInt::get(pExp32->getType(), 0xFF));
    Value* pIsNan = CreateAnd(pIsNanInf, CreateICmpNE(pMant32, Constant::getNullValue(pMant32->getType())));

    // inf16 = sign16 | 0x7C00
    Value* pInf16 = CreateOr(pSign16, ConstantInt::get(pSign16->getType(), 0x7C00));

    // nan16 = sign16 | 0x7C00 | (mant32 >> 13) | 1
    Value* pNan16 = CreateOr(CreateOr(pInf16, CreateLShr(pMant32, ConstantInt::get(pMant32->getType(), 13))),
                             ConstantInt::get(pMant32->getType(), 1));

    Value* pIsTooSmall = CreateICmpSLT(pExp16, ConstantInt::get(pExp16->getType(), -10));
    Value* pIsDenorm = CreateICmpSLE(pExp16, Constant::getNullValue(pExp16->getType()));

    // Calculate how many bits to discard from end of mantissa. Normally 13, but (14 - exp16) if denorm.
    // Also explicitly set implicit top set bit in mantissa if it is denorm.
    Value* pNumBitsToDiscard = CreateSelect(pIsDenorm,
                                            CreateSub(ConstantInt::get(pExp16->getType(), 14), pExp16),
                                            ConstantInt::get(pExp16->getType(), 13));
    pMant32 = CreateSelect(pIsDenorm, CreateOr(pMant32, ConstantInt::get(pMant32->getType(), 0x800000)), pMant32);

    // Ensure tiebreak-to-even by adding lowest nondiscarded bit to input mantissa.
    Constant* pOne = ConstantInt::get(pMant32->getType(), 1);
    pMant32 = CreateAdd(pMant32, CreateAnd(CreateLShr(pMant32, pNumBitsToDiscard), pOne));

    // Calculate amount to add to do rounding: ((1 << numBitsToDiscard) - 1) >> 1)
    Value* pRounder = CreateLShr(CreateSub(CreateShl(pOne, pNumBitsToDiscard), pOne), pOne);

    // Add rounder amount and discard bits.
    Value* pMant16 = CreateLShr(CreateAdd(pMant32, pRounder), pNumBitsToDiscard);

    // Combine exponent. Do this with an add, so that, if the rounding overflowed, the exponent automatically
    // gets incremented.
    pExp16 = CreateSelect(pIsDenorm, Constant::getNullValue(pExp16->getType()), pExp16);
    Value* pCombined16 = CreateAdd(pMant16, CreateShl(pExp16, ConstantInt::get(pMant16->getType(), 10)));

    // Zero if underflow.
    pCombined16 = CreateSelect(pIsTooSmall, Constant::getNullValue(pCombined16->getType()), pCombined16);

    // Check if the exponent is now too big.
    pIsNanInf = CreateOr(pIsNanInf, CreateICmpUGE(pCombined16, ConstantInt::get(pCombined16->getType(), 0x7C00)));

    // Combine in the sign. This gives the final result for zero, normals and denormals.
    pCombined16 = CreateOr(pCombined16, pSign16);

    // Select in inf or nan as appropriate.
    pCombined16 = CreateSelect(pIsNanInf, pInf16, pCombined16);
    pCombined16 = CreateSelect(pIsNan, pNan16, pCombined16);

    // Return as (vector of) half.
    return CreateBitCast(CreateTrunc(pCombined16, GetConditionallyVectorizedTy(getInt16Ty(), pDestTy)), pDestTy, instName);
}

// =====================================================================================================================
// Create quantize operation: truncates float (or vector) value to a value that is representable by a half.
Value* BuilderImplArith::CreateQuantizeToFp16(
    Value*        pValue,     // [in] Input value (float or float vector)
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    assert(pValue->getType()->getScalarType()->isFloatTy());

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
    if (pDivisor->getType()->getScalarType()->isIntegerTy(32) &&
        (GetPipelineState()->GetTargetInfo().GetGpuWorkarounds().gfx10.disableI32ModToI16Mod))
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
                Value* pPc = CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
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

    Value* pSrem = CreateSRem(pDividend, pDivisor);
    Value* pDivisorPlusSrem = CreateAdd(pDivisor, pSrem);
    Value* pIsDifferentSign = CreateICmpSLT(CreateXor(pDividend, pDivisor),
                                            Constant::getNullValue(pDividend->getType()));
    Value* pRemainderNotZero = CreateICmpNE(pSrem, Constant::getNullValue(pSrem->getType()));
    Value* pResultNeedsAddDivisor = CreateAnd(pIsDifferentSign, pRemainderNotZero);
    return CreateSelect(pResultNeedsAddDivisor, pDivisorPlusSrem, pSrem, instName);
}

// =====================================================================================================================
// Create FP modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
Value* BuilderImplArith::CreateFMod(
    Value*        pDividend,  // [in] Dividend value
    Value*        pDivisor,   // [in] Divisor value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pQuotient = CreateFMul(CreateFDiv(ConstantFP::get(pDivisor->getType(), 1.0), pDivisor), pDividend);
    Value* pFloor = CreateUnaryIntrinsic(Intrinsic::floor, pQuotient);
    return CreateFSub(pDividend, CreateFMul(pDivisor, pFloor), instName);
}

// =====================================================================================================================
// Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
Value* BuilderImplArith::CreateFma(
    Value*        pA,         // [in] One value to multiply
    Value*        pB,         // [in] The other value to multiply
    Value*        pC,         // [in] The value to add to the product of A and B
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major <= 8)
    {
        // Pre-GFX9 version: Use fmuladd.
        return CreateIntrinsic(Intrinsic::fmuladd, pA->getType(), { pA, pB, pC }, nullptr, instName);
    }

    // GFX9+ version: Use fma.
    return CreateIntrinsic(Intrinsic::fma, pA->getType(), { pA, pB, pC }, nullptr, instName);
}

// =====================================================================================================================
// Create a "tan" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateTan(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Constant* pOne = ConstantFP::get(pX->getType(), 1.0);
    Value* pSin = CreateUnaryIntrinsic(Intrinsic::sin, pX);
    Value* pCos = CreateUnaryIntrinsic(Intrinsic::cos, pX);
    return CreateFMul(pSin, CreateFDiv(pOne, pCos), instName);
}

// =====================================================================================================================
// Create an "asin" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateASin(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // Extend half to float.
    Type* pOrigTy = pX->getType();
    Type* pExtTy = pOrigTy;
    if (pExtTy->getScalarType()->isHalfTy())
    {
        pExtTy = GetConditionallyVectorizedTy(getFloatTy(), pExtTy);
        pX = CreateFPExt(pX, pExtTy);
    }

    // atan2(x, y), y = sqrt(1 - x * x)
    Value* pY = CreateFMul(pX, pX);
    Value* pOne = ConstantFP::get(pX->getType(), 1.0);
    pY = CreateFSub(pOne , pY);
    pY = CreateUnaryIntrinsic(Intrinsic::sqrt, pY);
    Value* pResult = CreateATan2(pX, pY);

    pResult = CreateFPTrunc(pResult, pOrigTy);
    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create an "acos" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateACos(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // Extend half to float.
    Type* pOrigTy = pX->getType();
    Type* pExtTy = pOrigTy;
    if (pExtTy->getScalarType()->isHalfTy())
    {
        pExtTy = GetConditionallyVectorizedTy(getFloatTy(), pExtTy);
        pX = CreateFPExt(pX, pExtTy);
    }

    // acos coefficient p0 = 0.08132463
    auto pCoefP0 = GetFpConstant(pX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FB4D1B0E0000000)));
    // acos coefficient p1 = -0.02363318
    auto pCoefP1 = GetFpConstant(pX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBF98334BE0000000)));

    Value* pResult = ASinACosCommon(pX, pCoefP0, pCoefP1);

    pResult = CreateFSub(GetPiByTwo(pResult->getType()), pResult);
    pResult = CreateFPTrunc(pResult, pOrigTy);
    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Common code for asin and acos
Value* BuilderImplArith::ASinACosCommon(
    Value*        pX,         // [in] Input value X
    Constant*     pCoefP0,    // [in] p0 coefficient
    Constant*     pCoefP1)    // [in] p1 coefficient
{
    // asin(x) = sgn(x) * (PI/2 - sqrt(1 - |x|) * (PI/2 + |x| * (PI/4 - 1 + |x| * (p0 + |x| * p1))))
    // acos(x) = PI/2 - the same, but with slightly different coefficients
    Value* pAbsInValue = CreateUnaryIntrinsic(Intrinsic::fabs, pX);
    Value* pResult = CreateFMul(pAbsInValue, pCoefP1);
    pResult = CreateFAdd(pResult, pCoefP0);
    pResult = CreateFMul(pAbsInValue, pResult);
    pResult = CreateFAdd(pResult, GetPiByFourMinusOne(pX->getType()));
    pResult = CreateFMul(pAbsInValue, pResult);
    pResult = CreateFAdd(pResult, GetPiByTwo(pX->getType()));

    Value* pSqrtTerm = CreateUnaryIntrinsic(Intrinsic::sqrt,
                                            CreateFSub(ConstantFP::get(pX->getType(), 1.0), pAbsInValue));
    pResult = CreateFMul(pSqrtTerm, pResult);
    pResult = CreateFSub(GetPiByTwo(pX->getType()), pResult);
    Value* pSign = CreateFSign(pX);
    return CreateFMul(pSign, pResult);
}

// =====================================================================================================================
// Create an "atan" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateATan(
    Value*        pYOverX,    // [in] Input value Y/X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // atan(x) = x - x^3 / 3 + x^5 / 5 - x^7 / 7 + x^9 / 9 - x^11 / 11, |x| <= 1.0
    // x = min(1.0, x) / max(1.0, x), make |x| <= 1.0
    Constant* pZero = Constant::getNullValue(pYOverX->getType());
    Constant* pOne = ConstantFP::get(pYOverX->getType(), 1.0);

    Value* pAbsX = CreateUnaryIntrinsic(Intrinsic::fabs, pYOverX);
    Value* pMax = CreateBinaryIntrinsic(Intrinsic::maxnum, pAbsX, pOne);
    Value* pMin = CreateBinaryIntrinsic(Intrinsic::minnum, pAbsX, pOne);
    Value* pBoundedX = CreateFMul(pMin, CreateFDiv(pOne, pMax));
    Value* pSquare = CreateFMul(pBoundedX, pBoundedX);
    Value* pCube = CreateFMul(pSquare, pBoundedX);
    Value* pPow5 = CreateFMul(pCube, pSquare);
    Value* pPow7 = CreateFMul(pPow5, pSquare);
    Value* pPow9 = CreateFMul(pPow7, pSquare);
    Value* pPow11 = CreateFMul(pPow9, pSquare);

    // coef1 = 0.99997932
    auto pCoef1 = GetFpConstant(pYOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FEFFFD4A0000000)));
    // coef3 = -0.33267564
    auto pCoef3 = GetFpConstant(pYOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBFD54A8EC0000000)));
    // coef5 = 0.19389249
    auto pCoef5 = GetFpConstant(pYOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FC8D17820000000)));
    // coef7 = -0.11735032
    auto pCoef7 = GetFpConstant(pYOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBFBE0AABA0000000)));
    // coef9 = 0.05368138
    auto pCoef9 = GetFpConstant(pYOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FAB7C2020000000)));
    // coef11 = -0.01213232
    auto pCoef11 = GetFpConstant(pYOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBF88D8D4A0000000)));

    Value* pTerm1 = CreateFMul(pBoundedX, pCoef1);
    Value* pTerm3 = CreateFMul(pCube, pCoef3);
    Value* pTerm5 = CreateFMul(pPow5, pCoef5);
    Value* pTerm7 = CreateFMul(pPow7, pCoef7);
    Value* pTerm9 = CreateFMul(pPow9, pCoef9);
    Value* pTerm11 = CreateFMul(pPow11, pCoef11);

    Value* pResult = CreateFAdd(pTerm1, pTerm3);
    pResult = CreateFAdd(pResult, pTerm5);
    pResult = CreateFAdd(pResult, pTerm7);
    pResult = CreateFAdd(pResult, pTerm9);
    Value* pPartialResult = CreateFAdd(pResult, pTerm11);
    pResult = CreateFMul(pPartialResult, ConstantFP::get(pYOverX->getType(), -2.0));
    pResult = CreateFAdd(pResult, GetPiByTwo(pYOverX->getType()));
    Value* pOutsideBound = CreateSelect(CreateFCmpOGT(pAbsX, pOne), pOne, pZero);
    pResult = CreateFMul(pOutsideBound, pResult);
    pResult = CreateFAdd(pPartialResult, pResult);
    return CreateFMul(pResult, CreateFSign(pYOverX));
}

// =====================================================================================================================
// Create an "atan2" operation for a scalar or vector float or half.
// Returns atan(Y/X) but in the correct quadrant for the input value signs.
Value* BuilderImplArith::CreateATan2(
    Value*        pY,         // [in] Input value Y
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // yox = (|x| == |y|) ? ((x == y) ? 1.0 : -1.0) : y/x
    //
    // p0 = sgn(y) * PI/2
    // p1 = sgn(y) * PI
    // atanyox = atan(yox)
    //
    // if (y != 0.0)
    //     if (x != 0.0)
    //         atan(y, x) = (x < 0.0) ? p1 + atanyox : atanyox
    //     else
    //         atan(y, x) = p0
    // else
    //     atan(y, x) = (x > 0.0) ? 0 : PI

    Constant* pZero = Constant::getNullValue(pY->getType());
    Constant* pOne = ConstantFP::get(pY->getType(), 1.0);
    Constant* pNegOne = ConstantFP::get(pY->getType(), -1.0);

    Value* pAbsX = CreateUnaryIntrinsic(Intrinsic::fabs, pX);
    Value* pAbsY = CreateUnaryIntrinsic(Intrinsic::fabs, pY);
    Value* pSignY = CreateFSign(pY);
    Value* pP0 = CreateFMul(pSignY, GetPiByTwo(pSignY->getType()));
    Value* pP1 = CreateFMul(pSignY, GetPi(pSignY->getType()));

    Value* pAbsXEqualsAbsY = CreateFCmpOEQ(pAbsX, pAbsY);
    // pOneIfEqual to (x == y) ? 1.0 : -1.0
    Value* pOneIfEqual = CreateSelect(CreateFCmpOEQ(pX, pY), pOne, pNegOne);

    Value* pYOverX = FDivFast(pY, pX);

    pYOverX = CreateSelect(pAbsXEqualsAbsY, pOneIfEqual, pYOverX);
    Value* pResult = CreateATan(pYOverX);
    Value* pAddP1 = CreateFAdd(pResult, pP1);
    pResult = CreateSelect(CreateFCmpOLT(pX, pZero), pAddP1, pResult);
    pResult = CreateSelect(CreateFCmpONE(pX, pZero), pResult, pP0);
    Value* pZeroOrPi = CreateSelect(CreateFCmpOGT(pX, pZero), pZero, GetPi(pX->getType()));
    pResult = CreateSelect(CreateFCmpONE(pY, pZero), pResult, pZeroOrPi, instName);
    return pResult;
}

// =====================================================================================================================
// Create a "sinh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateSinh(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // (e^x - e^(-x)) / 2.0
    // e^x = 2^(x * 1.442695)
    // 1/log(2) = 1.442695
    // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
    Constant* pZero = Constant::getNullValue(pX->getType());
    Constant* pHalf = ConstantFP::get(pX->getType(), 0.5);
    Value* pDivLog2 = CreateFMul(pX, GetRecipLog2(pX->getType()));
    Value* pNegDivLog2 = CreateFSub(pZero, pDivLog2);
    Value* pExp = CreateUnaryIntrinsic(Intrinsic::exp2, pDivLog2);
    Value* pExpNeg = CreateUnaryIntrinsic(Intrinsic::exp2, pNegDivLog2);
    Value* pResult = CreateFSub(pExp, pExpNeg);
    return CreateFMul(pResult, pHalf, instName);
}

// =====================================================================================================================
// Create a "cosh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateCosh(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // (e^x + e^(-x)) / 2.0
    // e^x = 2^(x * 1.442695)
    // 1/log(2) = 1.442695
    // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
    Value* pDivLog2 = CreateFMul(pX, GetRecipLog2(pX->getType()));
    Value* pNegDivLog2 = CreateFSub(ConstantFP::get(pX->getType(), 0.0), pDivLog2);
    Value* pExp = CreateUnaryIntrinsic(Intrinsic::exp2, pDivLog2);
    Value* pExpNeg = CreateUnaryIntrinsic(Intrinsic::exp2, pNegDivLog2);
    Value* pResult = CreateFAdd(pExp, pExpNeg);
    return CreateFMul(pResult, ConstantFP::get(pX->getType(), 0.5), instName);
}

// =====================================================================================================================
// Create a "tanh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateTanh(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // sinh(x) / cosh(x)
    // (e^x - e^(-x))/(e^x + e^(-x))
    // 1/log(2) = 1.442695
    // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
    Value* pDivLog2 = CreateFMul(pX, GetRecipLog2(pX->getType()));
    Value* pNegDivLog2 = CreateFSub(ConstantFP::get(pX->getType(), 0.0), pDivLog2);
    Value* pExp = CreateUnaryIntrinsic(Intrinsic::exp2, pDivLog2);
    Value* pExpNeg = CreateUnaryIntrinsic(Intrinsic::exp2, pNegDivLog2);
    Value* pDoubleSinh = CreateFSub(pExp, pExpNeg);
    Value* pDoubleCosh = CreateFAdd(pExp, pExpNeg);
    Value* pResult = FDivFast(pDoubleSinh, pDoubleCosh);
    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create an "asinh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateASinh(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // ln(x + sqrt(x*x + 1))
    //             / ln(x + sqrt(x^2 + 1))      when x >= 0
    //  asinh(x) =
    //             \ -ln((sqrt(x^2 + 1)- x))    when x < 0
    Constant* pOne = ConstantFP::get(pX->getType(), 1.0);
    Constant* pNegOne = ConstantFP::get(pX->getType(), -1.0);

    Value* pSquare = CreateFMul(pX, pX);
    Value* pSqrt = CreateUnaryIntrinsic(Intrinsic::sqrt, CreateFAdd(pSquare, pOne));
    Value* pIsNonNegative = CreateFCmpOGE(pX, Constant::getNullValue(pX->getType()));
    Value* pSign = CreateSelect(pIsNonNegative, pOne, pNegOne);
    Value* pAbs = CreateFMul(pX, pSign);
    Value* pResult = CreateFAdd(pSqrt, pAbs);
    pResult = CreateUnaryIntrinsic(Intrinsic::log2, pResult);
    pResult = CreateFMul(pResult, GetLog2(pX->getType()));
    return CreateFMul(pResult, pSign, instName);
}

// =====================================================================================================================
// Create an "acosh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateACosh(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // ln(x + sqrt(x*x - 1))
    // x should >= 1, undefined < 1
    Constant* pOne = ConstantFP::get(pX->getType(), 1.0);

    Value* pSquare = CreateFMul(pX, pX);
    Value* pSqrt = CreateUnaryIntrinsic(Intrinsic::sqrt, CreateFSub(pSquare, pOne));
    Value* pResult = CreateFAdd(pX, pSqrt);
    pResult = CreateUnaryIntrinsic(Intrinsic::log2, pResult);
    return CreateFMul(pResult, GetLog2(pX->getType()));
}

// =====================================================================================================================
// Create an "atanh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateATanh(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // ln((x + 1)/( 1 - x)) * 0.5f;
    // |x| <1, undefined |x| >= 1
    Constant* pOne = ConstantFP::get(pX->getType(), 1.0);
    Value* pOnePlusX = CreateFAdd(pX, pOne);
    Value* pOneMinusX = CreateFSub(pOne, pX);
    Value* pResult = CreateFMul(pOnePlusX, CreateFDiv(pOne, pOneMinusX));
    pResult = CreateUnaryIntrinsic(Intrinsic::log2, pResult);
    return CreateFMul(pResult, GetHalfLog2(pX->getType()), instName);
}

// =====================================================================================================================
// Create a "power" operation for a scalar or vector float or half, calculating X ^ Y
Value* BuilderImplArith::CreatePower(
    Value*        pX,         // [in] Input value X
    Value*        pY,         // [in] Input value Y
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (pX == ConstantFP::get(pX->getType(), 2.0))
    {
        return CreateUnaryIntrinsic(Intrinsic::exp2, pY, nullptr, instName);
    }

    // llvm.pow only works with (vector of) float.
    if (pX->getType()->getScalarType()->isFloatTy())
    {
        return CreateBinaryIntrinsic(Intrinsic::pow, pX, pY, nullptr, instName);
    }

    // pow(x, y) = exp2(y * log2(x))
    Value *pLog = CreateUnaryIntrinsic(Intrinsic::log2, pX);
    return CreateUnaryIntrinsic(Intrinsic::exp2, CreateFMul(pY, pLog), nullptr, instName);
}

// =====================================================================================================================
// Create an "exp" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateExp(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return CreateUnaryIntrinsic(Intrinsic::exp2,
                                CreateFMul(pX, GetRecipLog2(pX->getType())),
                                nullptr,
                                instName);
}

// =====================================================================================================================
// Create a "log" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateLog(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pLog = CreateUnaryIntrinsic(Intrinsic::log2, pX);
    return CreateFMul(pLog, GetLog2(pX->getType()), instName);
}

// =====================================================================================================================
// Create an inverse square root operation for a scalar or vector FP value.
Value* BuilderImplArith::CreateInverseSqrt(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return CreateFDiv(ConstantFP::get(pX->getType(), 1.0),
                      CreateUnaryIntrinsic(Intrinsic::sqrt, pX),
                      instName);
}

// =====================================================================================================================
// Create "signed integer abs" operation for a scalar or vector integer value.
Value* BuilderImplArith::CreateSAbs(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pNegX = CreateNeg(pX);
    Value* pIsPositive = CreateICmpSGT(pX, pNegX);
    return CreateSelect(pIsPositive, pX, pNegX, instName);
}

// =====================================================================================================================
// Create "fsign" operation for a scalar or vector floating-point type, returning -1.0, 0.0 or +1.0 if the input
// value is negative, zero or positive.
Value* BuilderImplArith::CreateFSign(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pIsPositive = CreateFCmpOGT(pX, Constant::getNullValue(pX->getType()));
    Value* pPartialResult = CreateSelect(pIsPositive, ConstantFP::get(pX->getType(), 1.0), pX);
    Value* pIsNonNegative = CreateFCmpOGE(pPartialResult, Constant::getNullValue(pX->getType()));
    return CreateSelect(pIsNonNegative, pPartialResult, ConstantFP::get(pX->getType(), -1.0), instName);
}

// =====================================================================================================================
// Create "ssign" operation for a scalar or vector integer type, returning -1, 0 or +1 if the input
// value is negative, zero or positive.
Value* BuilderImplArith::CreateSSign(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pIsPositive = CreateICmpSGT(pX, Constant::getNullValue(pX->getType()));
    Value* pPartialResult = CreateSelect(pIsPositive, ConstantInt::get(pX->getType(), 1, true), pX);
    Value* pIsNonNegative = CreateICmpSGE(pPartialResult, Constant::getNullValue(pX->getType()));
    return CreateSelect(pIsNonNegative, pPartialResult, ConstantInt::get(pX->getType(), -1, true), instName);
}

// =====================================================================================================================
// Create "fract" operation for a scalar or vector floating-point type, returning x - floor(x).
Value* BuilderImplArith::CreateFract(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // We need to scalarize this ourselves.
    Value* pResult = Scalarize(pX,
                               [this](Value* pX)
                               {
                                  return CreateIntrinsic(Intrinsic::amdgcn_fract, pX->getType(), pX);
                               });
    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create "smoothStep" operation. Result is 0.0 if x <= edge0 and 1.0 if x >= edge1 and performs smooth Hermite
// interpolation between 0 and 1 when edge0 < x < edge1. This is equivalent to:
// t * t * (3 - 2 * t), where t = clamp ((x - edge0) / (edge1 - edge0), 0, 1)
// Result is undefined if edge0 >= edge1.
Value* BuilderImplArith::CreateSmoothStep(
    Value*        pEdge0,     // [in] Edge0 value
    Value*        pEdge1,     // [in] Edge1 value
    Value*        pX,         // [in] X (input) value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (pEdge0->getType()->getScalarType()->isHalfTy())
    {
        // Enabling fast math flags for half type here causes test problems.
        // TODO: Investigate this further.
        clearFastMathFlags();
    }
    Value* pDiff = CreateFSub(pX, pEdge0);
    Constant* pOne = ConstantFP::get(pX->getType(), 1.0);
    Value* pT = CreateFMul(pDiff, CreateFDiv(pOne, CreateFSub(pEdge1, pEdge0)));
    pT = CreateFClamp(pT, Constant::getNullValue(pT->getType()), pOne);
    Value* pTSquared = CreateFMul(pT, pT);
    Value* pTerm = CreateFAdd(ConstantFP::get(pT->getType(), 3.0),
                              CreateFMul(ConstantFP::get(pT->getType(), -2.0), pT));
    return CreateFMul(pTSquared, pTerm, instName);
}

// =====================================================================================================================
// Create "ldexp" operation: given an FP mantissa and int exponent, build an FP value
Value* BuilderImplArith::CreateLdexp(
    Value*        pX,         // [in] Mantissa
    Value*        pExp,       // [in] Exponent
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // Ensure exponent is i32.
    if (pExp->getType()->getScalarType()->isIntegerTy(16))
    {
        pExp = CreateSExt(pExp, GetConditionallyVectorizedTy(getInt32Ty(), pExp->getType()));
    }
    else if (pExp->getType()->getScalarType()->isIntegerTy(64))
    {
        pExp = CreateTrunc(pExp, GetConditionallyVectorizedTy(getInt32Ty(), pExp->getType()));
    }

    // We need to scalarize this ourselves.
    Value* pResult = Scalarize(pX,
                               pExp,
                               [this](Value* pX, Value* pExp)
                               {
                                  return CreateIntrinsic(Intrinsic::amdgcn_ldexp, pX->getType(), { pX, pExp });
                               });
    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create "extract significand" operation: given an FP scalar or vector value, return the significand in the range
// [0.5,1.0), of the same type as the input. If the input is 0, the result is 0. If the input is infinite or NaN,
// the result is undefined.
Value* BuilderImplArith::CreateExtractSignificand(
    Value*        pValue,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // We need to scalarize this ourselves.
    Value* pMant = Scalarize(pValue,
                             [this](Value* pValue)
                             {
                                return CreateIntrinsic(Intrinsic::amdgcn_frexp_mant, pValue->getType(), pValue);
                             });
    pMant->setName(instName);
    return pMant;
}

// =====================================================================================================================
// Create "extract exponent" operation: given an FP scalar or vector value, return the exponent as a signed integer.
// If the input is (vector of) half, the result type is (vector of) i16, otherwise it is (vector of) i32.
// If the input is 0, the result is 0. If the input is infinite or NaN, the result is undefined.
Value* BuilderImplArith::CreateExtractExponent(
    Value*        pValue,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // We need to scalarize this ourselves.
    Type* pExpTy = pValue->getType()->getScalarType()->isHalfTy() ? getInt16Ty() : getInt32Ty();
    Value* pExp = Scalarize(pValue,
                            [this, pExpTy](Value* pValue)
                            {
                                return CreateIntrinsic(Intrinsic::amdgcn_frexp_exp,
                                                       { pExpTy, pValue->getType() },
                                                       pValue);
                            });
    pExp->setName(instName);
    return pExp;
}

// =====================================================================================================================
// Create vector cross product operation. Inputs must be <3 x FP>
Value* BuilderImplArith::CreateCrossProduct(
    Value*        pX,         // [in] Input value X
    Value*        pY,         // [in] Input value Y
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    assert((pX->getType() == pY->getType()) && (pX->getType()->getVectorNumElements() == 3));

    Value* pLeft = UndefValue::get(pX->getType());
    Value* pRight = UndefValue::get(pX->getType());
    for (uint32_t idx = 0; idx != 3; ++idx)
    {
        pLeft = CreateInsertElement(pLeft,
                                    CreateFMul(CreateExtractElement(pX, (idx + 1) % 3),
                                               CreateExtractElement(pY, (idx + 2) % 3)),
                                    idx);
        pRight = CreateInsertElement(pRight,
                                     CreateFMul(CreateExtractElement(pX, (idx + 2) % 3),
                                                CreateExtractElement(pY, (idx + 1) % 3)),
                                     idx);
    }
    return CreateFSub(pLeft, pRight, instName);
}

// =====================================================================================================================
// Create FP scalar/vector normalize operation: returns a scalar/vector with the same direction and magnitude 1.
Value* BuilderImplArith::CreateNormalizeVector(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (isa<VectorType>(pX->getType()) == false)
    {
        // For a scalar, just return -1.0 or +1.0.
        Value* pIsPositive = CreateFCmpOGT(pX, Constant::getNullValue(pX->getType()));
        return CreateSelect(pIsPositive,
                            ConstantFP::get(pX->getType(), 1.0),
                            ConstantFP::get(pX->getType(), -1.0),
                            instName);
    }

    // For a vector, divide by the length.
    Value* pDot = CreateDotProduct(pX, pX);
    Value* pSqrt = CreateIntrinsic(Intrinsic::sqrt, pDot->getType(), pDot);
    Value* pRsq = CreateFDiv(ConstantFP::get(pSqrt->getType(), 1.0), pSqrt);
    // We use fmul.legacy for float so that a zero vector is normalized to a zero vector,
    // rather than NaNs. We must scalarize it ourselves.
    Value* pResult = Scalarize(pX,
                               [this, pRsq](Value* pX) -> Value*
                               {
                                  if (pRsq->getType()->isFloatTy())
                                  {
                                      return CreateIntrinsic(Intrinsic::amdgcn_fmul_legacy, {}, { pX, pRsq });
                                  }
                                  return CreateFMul(pX, pRsq);
                               });
    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create "face forward" operation: given three FP scalars/vectors {N, I, Nref}, if the dot product of
// Nref and I is negative, the result is N, otherwise it is -N
Value* BuilderImplArith::CreateFaceForward(
    Value*        pN,         // [in] Input value "N"
    Value*        pI,         // [in] Input value "I"
    Value*        pNref,      // [in] Input value "Nref"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pDot = CreateDotProduct(pI, pNref);
    Value* pIsDotNegative = CreateFCmpOLT(pDot, Constant::getNullValue(pDot->getType()));
    Value* pNegN = CreateFSub(Constant::getNullValue(pN->getType()), pN);
    return CreateSelect(pIsDotNegative, pN, pNegN, instName);
}

// =====================================================================================================================
// Create "reflect" operation. For the incident vector I and normalized surface orientation N, the result is
// the reflection direction:
// I - 2 * dot(N, I) * N
Value* BuilderImplArith::CreateReflect(
    Value*        pI,         // [in] Input value "I"
    Value*        pN,         // [in] Input value "N"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* pDot = CreateDotProduct(pN, pI);
    pDot = CreateFMul(pDot, ConstantFP::get(pDot->getType(), 2.0));
    if (auto pVecTy = dyn_cast<VectorType>(pN->getType()))
    {
        pDot = CreateVectorSplat(pVecTy->getNumElements(), pDot);
    }
    return CreateFSub(pI, CreateFMul(pDot, pN), instName);
}

// =====================================================================================================================
// Create "refract" operation. For the normalized incident vector I, normalized surface orientation N and ratio
// of indices of refraction eta, the result is the refraction vector:
// k = 1.0 - eta * eta * (1.0 - dot(N,I) * dot(N,I))
// If k < 0.0 the result is 0.0.
// Otherwise, the result is eta * I - (eta * dot(N,I) + sqrt(k)) * N
Value* BuilderImplArith::CreateRefract(
    Value*        pI,         // [in] Input value "I"
    Value*        pN,         // [in] Input value "N"
    Value*        pEta,       // [in] Input value "eta"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Constant* pOne = ConstantFP::get(pEta->getType(), 1.0);
    Value* pDot = CreateDotProduct(pI, pN);
    Value* pDotSqr = CreateFMul(pDot, pDot);
    Value* pE1 = CreateFSub(pOne, pDotSqr);
    Value* pE2 = CreateFMul(pEta, pEta);
    Value* pE3 = CreateFMul(pE1, pE2);
    Value* pK = CreateFSub(pOne, pE3);
    Value* pKSqrt = CreateUnaryIntrinsic(Intrinsic::sqrt, pK);
    Value* pEtaDot = CreateFMul(pEta, pDot);
    Value* pInnt = CreateFAdd(pEtaDot, pKSqrt);

    if (auto pVecTy = dyn_cast<VectorType>(pN->getType()))
    {
        pEta = CreateVectorSplat(pVecTy->getNumElements(), pEta);
        pInnt = CreateVectorSplat(pVecTy->getNumElements(), pInnt);
    }
    pI = CreateFMul(pI, pEta);
    pN = CreateFMul(pN, pInnt);
    Value* pS = CreateFSub(pI, pN);
    Value* pCon = CreateFCmpOLT(pK, Constant::getNullValue(pK->getType()));
    return CreateSelect(pCon, Constant::getNullValue(pS->getType()), pS);
}

// =====================================================================================================================
// Create "fclamp" operation, returning min(max(x, minVal), maxVal). Result is undefined if minVal > maxVal.
// This honors the fast math flags; clear "nnan" in fast math flags in order to obtain the "NaN avoiding
// semantics" for the min and max where, if one input is NaN, it returns the other one.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFClamp(
    Value*        pX,         // [in] Value to clamp
    Value*        pMinVal,    // [in] Minimum of clamp range
    Value*        pMaxVal,    // [in] Maximum of clamp range
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // For float, and for half on GFX9+, we can use the fmed3 instruction.
    // But we can only do this if we do not need NaN preservation.
    Value* pResult = nullptr;
    if (getFastMathFlags().noNaNs() && (pX->getType()->getScalarType()->isFloatTy() ||
        ((GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major >= 9) && pX->getType()->getScalarType()->isHalfTy())))
    {
        pResult = Scalarize(pX,
                            pMinVal,
                            pMaxVal,
                            [this](Value* pX, Value* pMinVal, Value* pMaxVal)
                            {
                               return CreateIntrinsic(Intrinsic::amdgcn_fmed3,
                                                      pX->getType(),
                                                      { pX, pMinVal, pMaxVal });
                            });
        pResult->setName(instName);
    }
    else
    {
        // For half on GFX8 or earlier, or for double, use a combination of fmin and fmax.
        CallInst* pMax = CreateMaxNum(pX, pMinVal);
        pMax->setFastMathFlags(getFastMathFlags());
        CallInst* pMin = CreateMinNum(pMax, pMaxVal, instName);
        pMin->setFastMathFlags(getFastMathFlags());
        pResult = pMin;
    }

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major < 9)
    {
        pResult = Canonicalize(pResult);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create "fmin" operation, returning the minimum of two scalar or vector FP values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMin(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    CallInst* pMin = CreateMinNum(pValue1, pValue2);
    pMin->setFastMathFlags(getFastMathFlags());
    Value* pResult = pMin;

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major < 9)
    {
        pResult = Canonicalize(pResult);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create "fmax" operation, returning the maximum of two scalar or vector FP values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMax(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    CallInst* pMax = CreateMaxNum(pValue1, pValue2);
    pMax->setFastMathFlags(getFastMathFlags());
    Value* pResult = pMax;

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major < 9)
    {
        pResult = Canonicalize(pResult);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create "fmin3" operation, returning the minimum of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMin3(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    Value*        pValue3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    CallInst* pMin1 = CreateMinNum(pValue1, pValue2);
    pMin1->setFastMathFlags(getFastMathFlags());
    CallInst* pMin2 = CreateMinNum(pMin1, pValue3);
    pMin2->setFastMathFlags(getFastMathFlags());
    Value* pResult = pMin2;

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major < 9)
    {
        pResult = Canonicalize(pResult);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create "fmax3" operation, returning the maximum of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMax3(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    Value*        pValue3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    CallInst* pMax1 = CreateMaxNum(pValue1, pValue2);
    pMax1->setFastMathFlags(getFastMathFlags());
    CallInst* pMax2 = CreateMaxNum(pMax1, pValue3);
    pMax2->setFastMathFlags(getFastMathFlags());
    Value* pResult = pMax2;

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major < 9)
    {
        pResult = Canonicalize(pResult);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create "fmid3" operation, returning the middle one of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMid3(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    Value*        pValue3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // For float, and for half on GFX9+, we can use the fmed3 instruction.
    // But we can only do this if we do not need NaN preservation.
    Value* pResult = nullptr;
    if (getFastMathFlags().noNaNs() && (pValue1->getType()->getScalarType()->isFloatTy() ||
        ((GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major >= 9) && pValue1->getType()->getScalarType()->isHalfTy())))
    {
        pResult = Scalarize(pValue1,
                            pValue2,
                            pValue3,
                            [this](Value* pValue1, Value* pValue2, Value* pValue3)
                            {
                               return CreateIntrinsic(Intrinsic::amdgcn_fmed3,
                                                      pValue1->getType(),
                                                      { pValue1, pValue2, pValue3 });
                            });
    }
    else
    {
        // For half on GFX8 or earlier, use a combination of fmin and fmax.
        CallInst* pMin1 = CreateMinNum(pValue1, pValue2);
        pMin1->setFastMathFlags(getFastMathFlags());
        CallInst* pMax1 = CreateMaxNum(pValue1, pValue2);
        pMax1->setFastMathFlags(getFastMathFlags());
        CallInst* pMin2 = CreateMinNum(pMax1, pValue3);
        pMin2->setFastMathFlags(getFastMathFlags());
        CallInst* pMax2 = CreateMaxNum(pMin1, pMin2, instName);
        pMax2->setFastMathFlags(getFastMathFlags());
        pResult = pMax2;
    }

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major < 9)
    {
        pResult = Canonicalize(pResult);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Generate FP division, using fast fdiv for float to bypass optimization, and using fdiv 1.0 then fmul for
// half or double.
// TODO: IntrinsicsAMDGPU.td says amdgcn.fdiv.fast should not be used outside the backend.
Value* BuilderImplArith::FDivFast(
    Value* pNumerator,    // [in] Numerator
    Value* pDenominator)  // [in] Denominator
{
    if (pNumerator->getType()->getScalarType()->isFloatTy() == false)
    {
        return CreateFMul(pNumerator, CreateFDiv(ConstantFP::get(pDenominator->getType(), 1.0), pDenominator));
    }

    // We have to scalarize fdiv.fast ourselves.
    return Scalarize(pNumerator,
                     pDenominator,
                     [this](Value* pNumerator, Value* pDenominator) -> Value*
                     {
                        return CreateIntrinsic(Intrinsic::amdgcn_fdiv_fast, {}, { pNumerator, pDenominator });
                     });
}

// =====================================================================================================================
// Create "isInfinite" operation: return true if the supplied FP (or vector) value is infinity
Value* BuilderImplArith::CreateIsInf(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return CreateCallAmdgcnClass(pX, CmpClass::NegativeInfinity | CmpClass::PositiveInfinity, instName);
}

// =====================================================================================================================
// Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
Value* BuilderImplArith::CreateIsNaN(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // 0x001: signaling NaN, 0x002: quiet NaN
    return CreateCallAmdgcnClass(pX, CmpClass::SignalingNaN | CmpClass::QuietNaN, instName);
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
// Create an "insert bitfield" operation for a (vector of) integer type.
// Returns a value where the "pCount" bits starting at bit "pOffset" come from the least significant "pCount"
// bits in "pInsert", and remaining bits come from "pBase". The result is undefined if "pCount"+"pOffset" is
// more than the number of bits (per vector element) in "pBase" and "pInsert".
// If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
// width. The scalar type of "pOffset" and "pCount" must be integer, but can be different to that of "pBase"
// and "pInsert" (and different to each other too).
Value* BuilderImplArith::CreateInsertBitField(
    Value*        pBase,                // [in] Base value
    Value*        pInsert,              // [in] Value to insert (same type as base)
    Value*        pOffset,              // Bit number of least-significant end of bitfield
    Value*        pCount,               // Count of bits in bitfield
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    // Make pOffset and pCount vectors of the right integer type if necessary.
    if (auto pVecTy = dyn_cast<VectorType>(pBase->getType()))
    {
        if (isa<VectorType>(pOffset->getType()) == false)
        {
            pOffset = CreateVectorSplat(pVecTy->getNumElements(), pOffset);
        }
        if (isa<VectorType>(pCount->getType()) == false)
        {
            pCount = CreateVectorSplat(pVecTy->getNumElements(), pCount);
        }
    }
    pOffset = CreateZExtOrTrunc(pOffset, pBase->getType());
    pCount = CreateZExtOrTrunc(pCount, pBase->getType());

    Value* pBaseXorInsert = CreateXor(CreateShl(pInsert, pOffset), pBase);
    Constant* pOne = ConstantInt::get(pCount->getType(), 1);
    Value* pMask = CreateShl(CreateSub(CreateShl(pOne, pCount), pOne), pOffset);
    Value* pResult = CreateXor(CreateAnd(pBaseXorInsert, pMask), pBase);
    Value* pIsWholeField = CreateICmpEQ(pCount,
                                        ConstantInt::get(pCount->getType(),
                                                         pCount->getType()->getScalarType()->getPrimitiveSizeInBits()));
    return CreateSelect(pIsWholeField, pInsert, pResult, instName);
}

// =====================================================================================================================
// Create an "extract bitfield" operation for a (vector of) i32.
// Returns a value where the least significant "pCount" bits come from the "pCount" bits starting at bit
// "pOffset" in "pBase", and that is zero- or sign-extended (depending on "isSigned") to the rest of the value.
// If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
// width. The scalar type of "pOffset" and "pCount" must be integer, but can be different to that of "pBase"
// (and different to each other too).
Value* BuilderImplArith::CreateExtractBitField(
    Value*        pBase,                // [in] Base value
    Value*        pOffset,              // Bit number of least-significant end of bitfield
    Value*        pCount,               // Count of bits in bitfield
    bool          isSigned,             // True for a signed int bitfield extract, false for unsigned
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    // Make pOffset and pCount vectors of the right integer type if necessary.
    if (auto pVecTy = dyn_cast<VectorType>(pBase->getType()))
    {
        if (isa<VectorType>(pOffset->getType()) == false)
        {
            pOffset = CreateVectorSplat(pVecTy->getNumElements(), pOffset);
        }
        if (isa<VectorType>(pCount->getType()) == false)
        {
            pCount = CreateVectorSplat(pVecTy->getNumElements(), pCount);
        }
    }
    pOffset = CreateZExtOrTrunc(pOffset, pBase->getType());
    pCount = CreateZExtOrTrunc(pCount, pBase->getType());

    // For i32, we can use the amdgcn intrinsic and hence the instruction.
    if (pBase->getType()->getScalarType()->isIntegerTy(32))
    {
        Value* pIsWholeField = CreateICmpEQ(
                                    pCount,
                                    ConstantInt::get(pCount->getType(),
                                                     pCount->getType()->getScalarType()->getPrimitiveSizeInBits()));
        Value* pResult = Scalarize(pBase,
                                   pOffset,
                                   pCount,
                                   [this, isSigned](Value* pBase, Value* pOffset, Value* pCount)
                                   {
                                      return CreateIntrinsic(isSigned ? Intrinsic::amdgcn_sbfe : Intrinsic::amdgcn_ubfe,
                                                             pBase->getType(),
                                                             { pBase, pOffset, pCount });
                                   });
        pResult = CreateSelect(pIsWholeField, pBase, pResult);
        Value* pIsEmptyField = CreateICmpEQ(pCount, Constant::getNullValue(pCount->getType()));
        return CreateSelect(pIsEmptyField, Constant::getNullValue(pCount->getType()), pResult, instName);
    }

    // For other types, extract manually.
    Value* pShiftDown = CreateSub(ConstantInt::get(pBase->getType(),
                                                   pBase->getType()->getScalarType()->getPrimitiveSizeInBits()),
                                  pCount);
    Value* pShiftUp = CreateSub(pShiftDown, pOffset);
    Value* pResult = CreateShl(pBase, pShiftUp);
    if (isSigned)
    {
        pResult = CreateAShr(pResult, pShiftDown);
    }
    else
    {
        pResult = CreateLShr(pResult, pShiftDown);
    }
    Value* pIsZeroCount = CreateICmpEQ(pCount, Constant::getNullValue(pCount->getType()));
    return CreateSelect(pIsZeroCount, pCount, pResult, instName);
}

// =====================================================================================================================
// Create "find MSB" operation for a (vector of) signed i32. For a postive number, the result is the bit number of
// the most significant 1-bit. For a negative number, the result is the bit number of the most significant 0-bit.
// For a value of 0 or -1, the result is -1.
Value* BuilderImplArith::CreateFindSMsb(
    Value*        pValue,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    assert(pValue->getType()->getScalarType()->isIntegerTy(32));

    Constant* pNegOne = ConstantInt::get(pValue->getType(), -1);
    Value* pLeadingZeroCount = Scalarize(pValue,
                                         [this](Value* pValue)
                                         {
                                            return CreateUnaryIntrinsic(Intrinsic::amdgcn_sffbh, pValue);
                                         });
    Value* pBitOnePos = CreateSub(ConstantInt::get(pValue->getType(), 31), pLeadingZeroCount);
    Value* pIsNegOne = CreateICmpEQ(pValue, pNegOne);
    Value* pIsZero = CreateICmpEQ(pValue, Constant::getNullValue(pValue->getType()));
    Value* pIsNegOneOrZero = CreateOr(pIsNegOne, pIsZero);
    return CreateSelect(pIsNegOneOrZero, pNegOne, pBitOnePos, instName);
}

// =====================================================================================================================
// Create "fmix" operation, returning ( 1 - A ) * X + A * Y. Result would be FP scalar or vector value.
// Returns scalar, if and only if "pX", "pY" and "pA" are all scalars.
// Returns vector, if "pX" and "pY" are vector but "pA" is a scalar, under such condition, "pA" will be splatted.
// Returns vector, if "pX", "pY" and "pA" are all vectors.
// Note that when doing vector calculation, it means add/sub are element-wise between vectors, and the product will
// be Hadamard product.
Value* BuilderImplArith::CreateFMix(
    Value*        pX,        // [in] left Value
    Value*        pY,        // [in] right Value
    Value*        pA,        // [in] wight Value
    const Twine& instName)   // [in] Name to give instruction(s)
{
    Value* pYSubX = CreateFSub(pY, pX);
    if (auto pVectorResultTy = dyn_cast<VectorType>(pYSubX->getType()))
    {
        // pX, pY => vector, but pA => scalar
        if (isa<VectorType>(pA->getType()) == false)
        {
            pA = CreateVectorSplat(pVectorResultTy->getVectorNumElements(), pA);
        }
    }

    FastMathFlags fastMathFlags = getFastMathFlags();
    fastMathFlags.setNoNaNs();
    fastMathFlags.setAllowContract();
    CallInst* pResult = CreateIntrinsic(Intrinsic::fmuladd, pX->getType(), {pYSubX, pA, pX}, nullptr, instName);
    pResult->setFastMathFlags(fastMathFlags);

    return pResult;
}

// =====================================================================================================================
// Ensure result is canonicalized if the shader's FP mode is flush denorms. This is called on an FP result of an
// instruction that does not honor the hardware's FP mode, such as fmin/fmax/fmed on GFX8 and earlier.
Value* BuilderImplArith::Canonicalize(
    Value*  pValue)   // [in] Value to canonicalize
 {
    const auto& shaderMode = GetShaderModes()->GetCommonShaderMode(m_shaderStage);
    auto pDestTy = pValue->getType();
    FpDenormMode denormMode =
       pDestTy->getScalarType()->isHalfTy() ? shaderMode.fp16DenormMode :
       pDestTy->getScalarType()->isFloatTy() ? shaderMode.fp32DenormMode :
       pDestTy->getScalarType()->isDoubleTy() ? shaderMode.fp64DenormMode :
       FpDenormMode::DontCare;
    if ((denormMode == FpDenormMode::FlushOut) || (denormMode == FpDenormMode::FlushInOut))
    {
        // Has to flush denormals, insert canonicalize to make a MUL (* 1.0) forcibly
        pValue = CreateUnaryIntrinsic(Intrinsic::canonicalize, pValue);
    }
    return pValue;
}

