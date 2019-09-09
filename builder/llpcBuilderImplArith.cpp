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

    // asin coefficient p0 = 0.08656672
    auto pCoefP0 = GetFpConstant(pX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FB6293CA0000000)));
    // asin coefficient p1 = -0.03102955
    auto pCoefP1 = GetFpConstant(pX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBF9FC635E0000000)));

    Value* pResult = ASinACosCommon(pX, pCoefP0, pCoefP1);

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
// Create "fmed3" operation, returning the middle one of three scalar or vector float or half values.
Value* BuilderImplArith::CreateFMed3(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    Value*        pValue3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // For float, and for half on GFX9+, we can use the fmed3 instruction. We need to scalarize this ourselves.
    if (pValue1->getType()->getScalarType()->isFloatTy() || (getContext().GetGfxIpVersion().major >= 9))
    {
        Value* pResult = Scalarize(pValue1,
                                   pValue2,
                                   pValue3,
                                   [this](Value* pValue1, Value* pValue2, Value* pValue3)
                                   {
                                      return CreateIntrinsic(Intrinsic::amdgcn_fmed3,
                                                             pValue1->getType(),
                                                             { pValue1, pValue2, pValue3 });
                                   });
        pResult->setName(instName);
        return pResult;
    }

    // For half on GFX8 or earlier, use a combination of fmin and fmax.
    FastMathFlags fastMathFlags;
    fastMathFlags.setNoNaNs();
    CallInst* pMin1 = CreateMinNum(pValue1, pValue2);
    pMin1->setFastMathFlags(fastMathFlags);
    CallInst* pMax1 = CreateMaxNum(pValue1, pValue2);
    pMax1->setFastMathFlags(fastMathFlags);
    CallInst* pMin2 = CreateMinNum(pMax1, pValue3);
    pMin2->setFastMathFlags(fastMathFlags);
    CallInst* pMax2 = CreateMaxNum(pMin1, pMin2, instName);
    pMax2->setFastMathFlags(fastMathFlags);
    return pMax2;
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

