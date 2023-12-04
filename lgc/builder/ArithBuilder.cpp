/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ArithBuilder.cpp
 * @brief LLPC source file: implementation of arithmetic Builder methods
 ***********************************************************************************************************************
 */
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include <float.h>

#define DEBUG_TYPE "lgc-builder-impl-arith"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
// the given cube map texture coordinates. Returns <2 x float>.
//
// @param coord : Input coordinate <3 x float>
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateCubeFaceCoord(Value *coord, const Twine &instName) {
  Value *coordX = CreateExtractElement(coord, uint64_t(0));
  Value *coordY = CreateExtractElement(coord, 1);
  Value *coordZ = CreateExtractElement(coord, 2);
  Value *cubeMa = CreateIntrinsic(Intrinsic::amdgcn_cubema, {}, {coordX, coordY, coordZ}, nullptr);
  Value *recipMa = CreateFDiv(ConstantFP::get(getFloatTy(), 1.0), cubeMa);
  Value *cubeSc = CreateIntrinsic(Intrinsic::amdgcn_cubesc, {}, {coordX, coordY, coordZ}, nullptr);
  Value *scDivMa = CreateFMul(recipMa, cubeSc);
  Value *resultX = CreateFAdd(scDivMa, ConstantFP::get(getFloatTy(), 0.5));
  Value *cubeTc = CreateIntrinsic(Intrinsic::amdgcn_cubetc, {}, {coordX, coordY, coordZ}, nullptr);
  Value *tcDivMa = CreateFMul(recipMa, cubeTc);
  Value *resultY = CreateFAdd(tcDivMa, ConstantFP::get(getFloatTy(), 0.5));
  Value *result = CreateInsertElement(PoisonValue::get(FixedVectorType::get(getFloatTy(), 2)), resultX, uint64_t(0));
  result = CreateInsertElement(result, resultY, 1, instName);
  return result;
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
//
// @param coord : Input coordinate <3 x float>
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateCubeFaceIndex(Value *coord, const Twine &instName) {
  Value *coordX = CreateExtractElement(coord, uint64_t(0));
  Value *coordY = CreateExtractElement(coord, 1);
  Value *coordZ = CreateExtractElement(coord, 2);
  return CreateIntrinsic(Intrinsic::amdgcn_cubeid, {}, {coordX, coordY, coordZ}, nullptr, instName);
}

// =====================================================================================================================
// Create scalar or vector FP truncate operation with the given rounding mode.
// Currently the rounding mode is only implemented for float/double -> half conversion.
//
// @param value : Input value
// @param destTy : Type to convert to
// @param roundingMode : Rounding mode
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFpTruncWithRounding(Value *value, Type *destTy, RoundingMode roundingMode,
                                              const Twine &instName) {
  if (value->getType()->getScalarType()->isDoubleTy())
    value = CreateFPTrunc(value, BuilderBase::getConditionallyVectorizedTy(getFloatTy(), destTy));

  if (value->getType() == destTy)
    return value;

  assert(value->getType()->getScalarType()->isFloatTy() && destTy->getScalarType()->isHalfTy());

  if (roundingMode == RoundingMode::TowardZero) {
    // RTZ: Use cvt_pkrtz instruction.
    Value *result = scalarizeInPairs(value, [this](Value *inVec2) {
      Value *inVal0 = CreateExtractElement(inVec2, uint64_t(0));
      Value *inVal1 = CreateExtractElement(inVec2, 1);
      return CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {inVal0, inVal1});
    });
    result->setName(instName);
    return result;
  }

  if ((roundingMode == RoundingMode::TowardNegative) || (roundingMode == RoundingMode::TowardPositive)) {
    // RTN/RTP: Use fptrunc_round intrinsic.
    StringRef roundingModeStr = convertRoundingModeToStr(roundingMode).value();
    Value *roundingMode = MetadataAsValue::get(getContext(), MDString::get(getContext(), roundingModeStr));
    Value *result = scalarize(value, [=, this](Value *inValue) {
      return CreateIntrinsic(Intrinsic::fptrunc_round, {getHalfTy(), inValue->getType()}, {inValue, roundingMode});
    });
    result->setName(instName);
    return result;
  }

  // RTE.
  assert(roundingMode == RoundingMode::NearestTiesToEven);

  // float32: sign = [31], exponent = [30:23], mantissa = [22:0]
  // float16: sign = [15], exponent = [14:10], mantissa = [9:0]
  Value *bits32 = CreateBitCast(value, BuilderBase::getConditionallyVectorizedTy(getInt32Ty(), value->getType()));

  // sign16 = (bits32 >> 16) & 0x8000
  Value *sign16 = CreateAnd(CreateLShr(bits32, ConstantInt::get(bits32->getType(), 16)),
                            ConstantInt::get(bits32->getType(), 0x8000));

  // exp32 = (bits32 >> 23) & 0xFF
  Value *exp32 =
      CreateAnd(CreateLShr(bits32, ConstantInt::get(bits32->getType(), 23)), ConstantInt::get(bits32->getType(), 0xFF));

  // exp16 = exp32 - 127 + 15
  Value *exp16 = CreateSub(exp32, ConstantInt::get(exp32->getType(), (127 - 15)));

  // mant32 = bits32 & 0x7FFFFF
  Value *mant32 = CreateAnd(bits32, 0x7FFFFF);

  Value *isNanInf = CreateICmpEQ(exp32, ConstantInt::get(exp32->getType(), 0xFF));
  Value *isNan = CreateAnd(isNanInf, CreateICmpNE(mant32, Constant::getNullValue(mant32->getType())));

  // inf16 = sign16 | 0x7C00
  Value *inf16 = CreateOr(sign16, ConstantInt::get(sign16->getType(), 0x7C00));

  // nan16 = sign16 | 0x7C00 | (mant32 >> 13) | 1
  Value *nan16 = CreateOr(CreateOr(inf16, CreateLShr(mant32, ConstantInt::get(mant32->getType(), 13))),
                          ConstantInt::get(mant32->getType(), 1));

  Value *isTooSmall = CreateICmpSLT(exp16, ConstantInt::get(exp16->getType(), -10));
  Value *isDenorm = CreateICmpSLE(exp16, Constant::getNullValue(exp16->getType()));

  // Calculate how many bits to discard from end of mantissa. Normally 13, but (14 - exp16) if denorm.
  // Also explicitly set implicit top set bit in mantissa if it is denorm.
  Value *numBitsToDiscard = CreateSelect(isDenorm, CreateSub(ConstantInt::get(exp16->getType(), 14), exp16),
                                         ConstantInt::get(exp16->getType(), 13));
  mant32 = CreateSelect(isDenorm, CreateOr(mant32, ConstantInt::get(mant32->getType(), 0x800000)), mant32);

  // Ensure tiebreak-to-even by adding lowest nondiscarded bit to input mantissa.
  Constant *one = ConstantInt::get(mant32->getType(), 1);
  mant32 = CreateAdd(mant32, CreateAnd(CreateLShr(mant32, numBitsToDiscard), one));

  // Calculate amount to add to do rounding: ((1 << numBitsToDiscard) - 1) >> 1)
  Value *rounder = CreateLShr(CreateSub(CreateShl(one, numBitsToDiscard), one), one);

  // Add rounder amount and discard bits.
  Value *mant16 = CreateLShr(CreateAdd(mant32, rounder), numBitsToDiscard);

  // Combine exponent. Do this with an add, so that, if the rounding overflowed, the exponent automatically
  // gets incremented.
  exp16 = CreateSelect(isDenorm, Constant::getNullValue(exp16->getType()), exp16);
  Value *combined16 = CreateAdd(mant16, CreateShl(exp16, ConstantInt::get(mant16->getType(), 10)));

  // Zero if underflow.
  combined16 = CreateSelect(isTooSmall, Constant::getNullValue(combined16->getType()), combined16);

  // Check if the exponent is now too big.
  isNanInf = CreateOr(isNanInf, CreateICmpUGE(combined16, ConstantInt::get(combined16->getType(), 0x7C00)));

  // Combine in the sign. This gives the final result for zero, normals and denormals.
  combined16 = CreateOr(combined16, sign16);

  // Select in inf or nan as appropriate.
  combined16 = CreateSelect(isNanInf, inf16, combined16);
  combined16 = CreateSelect(isNan, nan16, combined16);

  // Return as (vector of) half.
  return CreateBitCast(CreateTrunc(combined16, BuilderBase::getConditionallyVectorizedTy(getInt16Ty(), destTy)), destTy,
                       instName);
}

// =====================================================================================================================
// Create quantize operation: truncates float (or vector) value to a value that is representable by a half.
//
// @param value : Input value (float or float vector)
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateQuantizeToFp16(Value *value, const Twine &instName) {
  assert(value->getType()->getScalarType()->isFloatTy());

  Constant *zero = Constant::getNullValue(value->getType());
  // 2^-15 (normalized float16 minimum)
  Constant *minNormalizedHalf = ConstantFP::get(value->getType(), 1.0 / 32768.0);

  Value *trunc = CreateFPTrunc(value, BuilderBase::getConditionallyVectorizedTy(getHalfTy(), value->getType()));
  Value *ext = CreateFPExt(trunc, value->getType());
  Value *abs = CreateIntrinsic(Intrinsic::fabs, ext->getType(), ext);
  Value *isLessThanMin = CreateFCmpOLT(abs, minNormalizedHalf);
  Value *isNotZero = CreateFCmpONE(abs, zero);
  Value *isDenorm = CreateAnd(isLessThanMin, isNotZero);
  Value *result = CreateSelect(isDenorm, zero, ext);

  // Check NaN.
  Value *isNan = CreateIsNaN(value);
  return CreateSelect(isNan, value, result, instName);
}

// =====================================================================================================================
// Create signed integer modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
//
// @param dividend : Dividend value
// @param divisor : Divisor value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSMod(Value *dividend, Value *divisor, const Twine &instName) {
  Value *srem = CreateSRem(dividend, divisor);
  Value *divisorPlusSrem = CreateAdd(divisor, srem);
  Value *isDifferentSign = CreateICmpSLT(CreateXor(dividend, divisor), Constant::getNullValue(dividend->getType()));
  Value *remainderNotZero = CreateICmpNE(srem, Constant::getNullValue(srem->getType()));
  Value *resultNeedsAddDivisor = CreateAnd(isDifferentSign, remainderNotZero);
  return CreateSelect(resultNeedsAddDivisor, divisorPlusSrem, srem, instName);
}

// =====================================================================================================================
// Create FP modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
//
// @param dividend : Dividend value
// @param divisor : Divisor value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFMod(Value *dividend, Value *divisor, const Twine &instName) {
  Value *quotient = CreateFMul(CreateFDiv(ConstantFP::get(divisor->getType(), 1.0), divisor), dividend);
  Value *floor = CreateUnaryIntrinsic(Intrinsic::floor, quotient);
  return CreateFSub(dividend, CreateFMul(divisor, floor), instName);
}

// =====================================================================================================================
// Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
//
// @param a : One value to multiply
// @param b : The other value to multiply
// @param c : The value to add to the product of A and B
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFma(Value *a, Value *b, Value *c, const Twine &instName) {
  // GFX9+ version: Use fma.
  return CreateIntrinsic(Intrinsic::fma, a->getType(), {a, b, c}, nullptr, instName);
}

// =====================================================================================================================
// Create a "tan" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateTan(Value *x, const Twine &instName) {
  Constant *one = ConstantFP::get(x->getType(), 1.0);
  Value *sin = CreateUnaryIntrinsic(Intrinsic::sin, x);
  Value *cos = CreateUnaryIntrinsic(Intrinsic::cos, x);
  return CreateFMul(sin, CreateFDiv(one, cos), instName);
}

// =====================================================================================================================
// Create an "asin" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateASin(Value *x, const Twine &instName) {
  // Extend half to float.
  Type *origTy = x->getType();
  Type *extTy = origTy;
  if (extTy->getScalarType()->isHalfTy()) {
    extTy = BuilderBase::getConditionallyVectorizedTy(getFloatTy(), extTy);
    x = CreateFPExt(x, extTy);
  }

  // atan2(x, y), y = sqrt(1 - x * x)
  Value *y = CreateFMul(x, x);
  Value *one = ConstantFP::get(x->getType(), 1.0);
  y = CreateFSub(one, y);
  y = CreateUnaryIntrinsic(Intrinsic::sqrt, y);
  Value *result = CreateATan2(x, y);

  result = CreateFPTrunc(result, origTy);
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create an "acos" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateACos(Value *x, const Twine &instName) {
  // Extend half to float.
  Type *origTy = x->getType();
  Type *extTy = origTy;
  if (extTy->getScalarType()->isHalfTy()) {
    extTy = BuilderBase::getConditionallyVectorizedTy(getFloatTy(), extTy);
    x = CreateFPExt(x, extTy);
  }

  // acos coefficient p0 = 0.08132463
  auto coefP0 = getFpConstant(x->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FB4D1B0E0000000)));
  // acos coefficient p1 = -0.02363318
  auto coefP1 = getFpConstant(x->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBF98334BE0000000)));

  Value *result = aSinACosCommon(x, coefP0, coefP1);

  result = CreateFSub(getPiByTwo(result->getType()), result);
  result = CreateFPTrunc(result, origTy);
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Common code for asin and acos
//
// @param x : Input value X
// @param coefP0 : p0 coefficient
// @param coefP1 : p1 coefficient
Value *BuilderImpl::aSinACosCommon(Value *x, Constant *coefP0, Constant *coefP1) {
  // asin(x) = sgn(x) * (PI/2 - sqrt(1 - |x|) * (PI/2 + |x| * (PI/4 - 1 + |x| * (p0 + |x| * p1))))
  // acos(x) = PI/2 - the same, but with slightly different coefficients
  Value *absInValue = CreateUnaryIntrinsic(Intrinsic::fabs, x);
  Value *result = CreateFMul(absInValue, coefP1);
  result = CreateFAdd(result, coefP0);
  result = CreateFMul(absInValue, result);
  result = CreateFAdd(result, getPiByFourMinusOne(x->getType()));
  result = CreateFMul(absInValue, result);
  result = CreateFAdd(result, getPiByTwo(x->getType()));

  Value *sqrtTerm = CreateUnaryIntrinsic(Intrinsic::sqrt, CreateFSub(ConstantFP::get(x->getType(), 1.0), absInValue));
  result = CreateFMul(sqrtTerm, result);
  result = CreateFSub(getPiByTwo(x->getType()), result);
  Value *sign = CreateFSign(x);
  return CreateFMul(sign, result);
}

// =====================================================================================================================
// Create an "atan" operation for a scalar or vector float or half.
//
// @param yOverX : Input value Y/X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateATan(Value *yOverX, const Twine &instName) {
  // atan(x) = x - x^3 / 3 + x^5 / 5 - x^7 / 7 + x^9 / 9 - x^11 / 11, |x| <= 1.0
  // x = min(1.0, x) / max(1.0, x), make |x| <= 1.0
  Constant *zero = Constant::getNullValue(yOverX->getType());
  Constant *one = ConstantFP::get(yOverX->getType(), 1.0);

  Value *absX = CreateUnaryIntrinsic(Intrinsic::fabs, yOverX);
  Value *max = CreateBinaryIntrinsic(Intrinsic::maxnum, absX, one);
  Value *min = CreateBinaryIntrinsic(Intrinsic::minnum, absX, one);
  Value *boundedX = CreateFMul(min, CreateFDiv(one, max));
  Value *square = CreateFMul(boundedX, boundedX);
  Value *cube = CreateFMul(square, boundedX);
  Value *pow5 = CreateFMul(cube, square);
  Value *pow7 = CreateFMul(pow5, square);
  Value *pow9 = CreateFMul(pow7, square);
  Value *pow11 = CreateFMul(pow9, square);

  // coef1 = 0.99997932
  auto coef1 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FEFFFD4A0000000)));
  // coef3 = -0.33267564
  auto coef3 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBFD54A8EC0000000)));
  // coef5 = 0.19389249
  auto coef5 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FC8D17820000000)));
  // coef7 = -0.11735032
  auto coef7 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBFBE0AABA0000000)));
  // coef9 = 0.05368138
  auto coef9 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FAB7C2020000000)));
  // coef11 = -0.01213232
  auto coef11 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBF88D8D4A0000000)));

  Value *term1 = CreateFMul(boundedX, coef1);
  Value *term3 = CreateFMul(cube, coef3);
  Value *term5 = CreateFMul(pow5, coef5);
  Value *term7 = CreateFMul(pow7, coef7);
  Value *term9 = CreateFMul(pow9, coef9);
  Value *term11 = CreateFMul(pow11, coef11);

  Value *result = CreateFAdd(term1, term3);
  result = CreateFAdd(result, term5);
  result = CreateFAdd(result, term7);
  result = CreateFAdd(result, term9);
  Value *partialResult = CreateFAdd(result, term11);
  result = CreateFMul(partialResult, ConstantFP::get(yOverX->getType(), -2.0));
  result = CreateFAdd(result, getPiByTwo(yOverX->getType()));
  Value *outsideBound = CreateSelect(CreateFCmpOGT(absX, one), one, zero);
  result = CreateFMul(outsideBound, result);
  result = CreateFAdd(partialResult, result);
  result = CreateFMul(result, CreateFSign(yOverX));
  return CreateSelect(CreateIsNaN(yOverX), ConstantFP::getNaN(yOverX->getType()), result);
}

// =====================================================================================================================
// Create an "atan2" operation for a scalar or vector float or half.
// Returns atan(Y/X) but in the correct quadrant for the input value signs.
//
// @param y : Input value Y
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateATan2(Value *y, Value *x, const Twine &instName) {
  // yox = (|x| == |y|) ? ((x == y) ? 1.0 : -1.0) : y/x
  //
  // p0 = sgn(y) * PI/2
  // p1 = sgn(y) * PI
  // p2 = copysign(PI, y)
  // atanyox = atan(yox)
  //
  // if (y != 0.0)
  //     if (x != 0.0)
  //         atan(y, x) = (x < 0.0) ? p1 + atanyox : atanyox
  //     else
  //         atan(y, x) = p0
  // else
  //     atan(y, x) = (x > 0.0) ? 0 : p2

  Constant *zero = Constant::getNullValue(y->getType());
  Constant *one = ConstantFP::get(y->getType(), 1.0);
  Constant *negOne = ConstantFP::get(y->getType(), -1.0);

  Value *absX = CreateUnaryIntrinsic(Intrinsic::fabs, x);
  Value *absY = CreateUnaryIntrinsic(Intrinsic::fabs, y);
  Value *signY = CreateFSign(y);
  Value *p0 = CreateFMul(signY, getPiByTwo(signY->getType()));
  Value *p1 = CreateFMul(signY, getPi(signY->getType()));
  Value *p2 = getPi(x->getType());
  if (!getFastMathFlags().noSignedZeros()) {
    // NOTE: According to the definition of atan(y, x), we might take the sign of y into consideration and follow such
    // computation:
    //                / -PI, when y = -0.0 and x < 0
    //   atan(y, x) =
    //                \ PI, when y = 0.0 and x < 0
    p2 = CreateCopySign(p2, y);
  }

  Value *absXEqualsAbsY = CreateFCmpOEQ(absX, absY);
  // oneIfEqual to (x == y) ? 1.0 : -1.0
  Value *oneIfEqual = CreateSelect(CreateFCmpOEQ(x, y), one, negOne);

  Value *yOverX = fDivFast(y, x);

  yOverX = CreateSelect(absXEqualsAbsY, oneIfEqual, yOverX);
  Value *result = CreateATan(yOverX);
  Value *addP1 = CreateFAdd(result, p1);
  result = CreateSelect(CreateFCmpOLT(x, zero), addP1, result);
  result = CreateSelect(CreateFCmpUNE(x, zero), result, p0);
  Value *zeroOrPi = CreateSelect(CreateFCmpOGT(x, zero), zero, p2);
  result = CreateSelect(CreateFCmpUNE(y, zero), result, zeroOrPi, instName);
  return result;
}

// =====================================================================================================================
// Create a "sinh" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSinh(Value *x, const Twine &instName) {
  // (e^x - e^(-x)) / 2.0
  // e^x = 2^(x * 1.442695)
  // 1/log(2) = 1.442695
  // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
  Constant *zero = Constant::getNullValue(x->getType());
  Constant *half = ConstantFP::get(x->getType(), 0.5);
  Value *divLog2 = CreateFMul(x, getRecipLog2(x->getType()));
  Value *negDivLog2 = CreateFSub(zero, divLog2);
  Value *exp = CreateUnaryIntrinsic(Intrinsic::exp2, divLog2);
  Value *expNeg = CreateUnaryIntrinsic(Intrinsic::exp2, negDivLog2);
  Value *result = CreateFSub(exp, expNeg);
  return CreateFMul(result, half, instName);
}

// =====================================================================================================================
// Create a "cosh" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateCosh(Value *x, const Twine &instName) {
  // (e^x + e^(-x)) / 2.0
  // e^x = 2^(x * 1.442695)
  // 1/log(2) = 1.442695
  // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
  Value *divLog2 = CreateFMul(x, getRecipLog2(x->getType()));
  Value *negDivLog2 = CreateFSub(ConstantFP::get(x->getType(), 0.0), divLog2);
  Value *exp = CreateUnaryIntrinsic(Intrinsic::exp2, divLog2);
  Value *expNeg = CreateUnaryIntrinsic(Intrinsic::exp2, negDivLog2);
  Value *result = CreateFAdd(exp, expNeg);
  return CreateFMul(result, ConstantFP::get(x->getType(), 0.5), instName);
}

// =====================================================================================================================
// Create a "tanh" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateTanh(Value *x, const Twine &instName) {
  // sinh(x) / cosh(x)
  // (e^x - e^(-x))/(e^x + e^(-x))
  // 1/log(2) = 1.442695
  // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
  Value *divLog2 = CreateFMul(x, getRecipLog2(x->getType()));
  Value *negDivLog2 = CreateFSub(ConstantFP::get(x->getType(), 0.0), divLog2);
  Value *exp = CreateUnaryIntrinsic(Intrinsic::exp2, divLog2);
  Value *expNeg = CreateUnaryIntrinsic(Intrinsic::exp2, negDivLog2);
  Value *doubleSinh = CreateFSub(exp, expNeg);
  Value *doubleCosh = CreateFAdd(exp, expNeg);
  Value *result = fDivFast(doubleSinh, doubleCosh);

  if (!getFastMathFlags().noInfs()) {
    // NOTE: If the fast math flags might have INFs, we should check the special case when the input is +INF or -INF.
    // According to the limit of tanh(x), we have following definitions:
    //                  / 1.0, when x -> +INF
    //   lim(tanh(x)) =
    //                  \ -1.0, when x -> -INF
    Value *one = ConstantFP::get(x->getType(), 1.0);
    Value *isInf = CreateIsInf(x);
    result = CreateSelect(isInf, CreateCopySign(one, x), result);
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create an "asinh" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateASinh(Value *x, const Twine &instName) {
  // ln(x + sqrt(x*x + 1))
  //             / ln(x + sqrt(x^2 + 1))      when x >= 0
  //  asinh(x) =
  //             \ -ln((sqrt(x^2 + 1)- x))    when x < 0
  Constant *one = ConstantFP::get(x->getType(), 1.0);
  Constant *negOne = ConstantFP::get(x->getType(), -1.0);

  Value *square = CreateFMul(x, x);
  Value *sqrt = CreateUnaryIntrinsic(Intrinsic::sqrt, CreateFAdd(square, one));
  Value *isNonNegative = CreateFCmpOGE(x, Constant::getNullValue(x->getType()));
  Value *sign = CreateSelect(isNonNegative, one, negOne);
  Value *abs = CreateFMul(x, sign);
  Value *result = CreateFAdd(sqrt, abs);
  result = CreateUnaryIntrinsic(Intrinsic::log2, result);
  result = CreateFMul(result, getLog2(x->getType()));
  return CreateFMul(result, sign, instName);
}

// =====================================================================================================================
// Create an "acosh" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateACosh(Value *x, const Twine &instName) {
  // ln(x + sqrt(x*x - 1))
  // x should >= 1, undefined < 1
  Constant *one = ConstantFP::get(x->getType(), 1.0);

  Value *square = CreateFMul(x, x);
  Value *sqrt = CreateUnaryIntrinsic(Intrinsic::sqrt, CreateFSub(square, one));
  Value *result = CreateFAdd(x, sqrt);
  result = CreateUnaryIntrinsic(Intrinsic::log2, result);
  return CreateFMul(result, getLog2(x->getType()));
}

// =====================================================================================================================
// Create an "atanh" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateATanh(Value *x, const Twine &instName) {
  // ln((x + 1)/( 1 - x)) * 0.5f;
  // |x| <1, undefined |x| >= 1
  Constant *one = ConstantFP::get(x->getType(), 1.0);
  Value *onePlusX = CreateFAdd(x, one);
  Value *oneMinusX = CreateFSub(one, x);
  Value *result = CreateFMul(onePlusX, CreateFDiv(one, oneMinusX));
  result = CreateUnaryIntrinsic(Intrinsic::log2, result);
  return CreateFMul(result, getHalfLog2(x->getType()), instName);
}

// =====================================================================================================================
// Create a "power" operation for a scalar or vector float or half, calculating X ^ Y
//
// @param x : Input value X
// @param y : Input value Y
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreatePower(Value *x, Value *y, const Twine &instName) {
  if (x == ConstantFP::get(x->getType(), 2.0))
    return CreateUnaryIntrinsic(Intrinsic::exp2, y, nullptr, instName);

  // llvm.pow only works with (vector of) float.
  if (x->getType()->getScalarType()->isFloatTy())
    return CreateBinaryIntrinsic(Intrinsic::pow, x, y, nullptr, instName);

  // pow(x, y) = exp2(y * log2(x))
  Value *log = CreateUnaryIntrinsic(Intrinsic::log2, x);
  return CreateUnaryIntrinsic(Intrinsic::exp2, CreateFMul(y, log), nullptr, instName);
}

// =====================================================================================================================
// Create an "exp" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateExp(Value *x, const Twine &instName) {
  return CreateUnaryIntrinsic(Intrinsic::exp2, CreateFMul(x, getRecipLog2(x->getType())), nullptr, instName);
}

// =====================================================================================================================
// Create a "log" operation for a scalar or vector float or half.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateLog(Value *x, const Twine &instName) {
  Value *log = CreateUnaryIntrinsic(Intrinsic::log2, x);
  return CreateFMul(log, getLog2(x->getType()), instName);
}

// =====================================================================================================================
// Create a square root operation for a scalar or vector FP value.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSqrt(Value *x, const Twine &instName) {
  return CreateUnaryIntrinsic(Intrinsic::sqrt, x, nullptr, instName);
}

// =====================================================================================================================
// Create a inverse square root operation for a scalar or vector FP value.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateInverseSqrt(Value *x, const Twine &instName) {
  if (x->getType()->getScalarType()->isDoubleTy()) {
    // NOTE: For double type, the intrinsic amdgcn_rsq doesn't have required precision, we resort to LLVM native
    // intrinsic sqrt since it will be expanded in backend with Goldschmidt's algorithm to improve the precision.
    return CreateFDiv(ConstantFP::get(x->getType(), 1.0), CreateUnaryIntrinsic(Intrinsic::sqrt, x));
  }

  Value *result = scalarize(x, [this](Value *x) { return CreateUnaryIntrinsic(Intrinsic::amdgcn_rsq, x); });
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "signed integer abs" operation for a scalar or vector integer value.
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSAbs(Value *x, const Twine &instName) {
  Value *negX = CreateNeg(x);
  Value *isPositive = CreateICmpSGT(x, negX);
  return CreateSelect(isPositive, x, negX, instName);
}

// =====================================================================================================================
// Create "fsign" operation for a scalar or vector floating-point type, returning -1.0, 0.0 or +1.0 if the input
// value is negative, zero or positive.
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFSign(Value *x, const Twine &instName) {
  Value *isPositive = CreateFCmpOGT(x, Constant::getNullValue(x->getType()));
  Value *partialResult = CreateSelect(isPositive, ConstantFP::get(x->getType(), 1.0), x);
  Value *isNonNegative = CreateFCmpOGE(partialResult, Constant::getNullValue(x->getType()));
  return CreateSelect(isNonNegative, partialResult, ConstantFP::get(x->getType(), -1.0), instName);
}

// =====================================================================================================================
// Create "ssign" operation for a scalar or vector integer type, returning -1, 0 or +1 if the input
// value is negative, zero or positive.
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSSign(Value *x, const Twine &instName) {
  Value *isPositive = CreateICmpSGT(x, Constant::getNullValue(x->getType()));
  Value *partialResult = CreateSelect(isPositive, ConstantInt::get(x->getType(), 1, true), x);
  Value *isNonNegative = CreateICmpSGE(partialResult, Constant::getNullValue(x->getType()));
  return CreateSelect(isNonNegative, partialResult, ConstantInt::get(x->getType(), -1, true), instName);
}

// =====================================================================================================================
// Create "fract" operation for a scalar or vector floating-point type, returning x - floor(x).
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFract(Value *x, const Twine &instName) {
  // We need to scalarize this ourselves.
  Value *result = scalarize(x, [this](Value *x) { return CreateIntrinsic(Intrinsic::amdgcn_fract, x->getType(), x); });
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "smoothStep" operation. Result is 0.0 if x <= edge0 and 1.0 if x >= edge1 and performs smooth Hermite
// interpolation between 0 and 1 when edge0 < x < edge1. This is equivalent to:
// t * t * (3 - 2 * t), where t = clamp ((x - edge0) / (edge1 - edge0), 0, 1)
// Result is undefined if edge0 >= edge1.
//
// @param edge0 : Edge0 value
// @param edge1 : Edge1 value
// @param x : X (input) value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSmoothStep(Value *edge0, Value *edge1, Value *x, const Twine &instName) {
  if (edge0->getType()->getScalarType()->isHalfTy()) {
    // Enabling fast math flags for half type here causes test problems.
    // TODO: Investigate this further.
    clearFastMathFlags();
  }
  Value *diff = CreateFSub(x, edge0);
  Constant *one = ConstantFP::get(x->getType(), 1.0);
  Value *t = CreateFMul(diff, CreateFDiv(one, CreateFSub(edge1, edge0)));
  t = CreateFClamp(t, Constant::getNullValue(t->getType()), one);
  Value *tSquared = CreateFMul(t, t);
  Value *term = CreateFAdd(ConstantFP::get(t->getType(), 3.0), CreateFMul(ConstantFP::get(t->getType(), -2.0), t));
  return CreateFMul(tSquared, term, instName);
}

// =====================================================================================================================
// Create "ldexp" operation: given an FP mantissa and int exponent, build an FP value
//
// @param x : Mantissa
// @param exp : Exponent
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateLdexp(Value *x, Value *exp, const Twine &instName) {
  // Ensure exponent is i32.
  if (exp->getType()->getScalarType()->isIntegerTy(16))
    exp = CreateSExt(exp, BuilderBase::getConditionallyVectorizedTy(getInt32Ty(), exp->getType()));
  else if (exp->getType()->getScalarType()->isIntegerTy(64))
    exp = CreateTrunc(exp, BuilderBase::getConditionallyVectorizedTy(getInt32Ty(), exp->getType()));

  // We need to scalarize this ourselves.
  Value *result = scalarize(x, exp, [this](Value *x, Value *exp) {
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 463519
    // Old version of the code
    Value *ldexp = CreateIntrinsic(Intrinsic::amdgcn_ldexp, x->getType(), {x, exp});
#else
    // New version of the code (also handles unknown version, which we treat as latest)
    Value *ldexp = CreateIntrinsic(x->getType(), Intrinsic::ldexp, {x, exp});
#endif
    if (x->getType()->getScalarType()->isDoubleTy()) {
      // NOTE: If LDEXP result is a denormal, we can flush it to zero. This is allowed. For double type, LDEXP
      // instruction does mantissa rounding instead of truncation, which is not expected by SPIR-V spec.
      auto exp = CreateExtractExponent(ldexp);
      // Exponent < DBL_MIN_EXP is denormal
      ldexp = CreateSelect(CreateICmpSLT(exp, ConstantInt::get(exp->getType(), DBL_MIN_EXP)),
                           ConstantFP::get(x->getType(), 0.0), ldexp);
    }
    return ldexp;
  });
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "extract significand" operation: given an FP scalar or vector value, return the significand in the range
// [0.5,1.0), of the same type as the input. If the input is 0, the result is 0. If the input is infinite or NaN,
// the result is undefined.
//
// @param value : Input value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateExtractSignificand(Value *value, const Twine &instName) {
  // We need to scalarize this ourselves.
  Value *mant = scalarize(
      value, [this](Value *value) { return CreateIntrinsic(Intrinsic::amdgcn_frexp_mant, value->getType(), value); });
  mant->setName(instName);
  return mant;
}

// =====================================================================================================================
// Create "extract exponent" operation: given an FP scalar or vector value, return the exponent as a signed integer.
// If the input is (vector of) half, the result type is (vector of) i16, otherwise it is (vector of) i32.
// If the input is 0, the result is 0. If the input is infinite or NaN, the result is undefined.
//
// @param value : Input value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateExtractExponent(Value *value, const Twine &instName) {
  // We need to scalarize this ourselves.
  Type *expTy = value->getType()->getScalarType()->isHalfTy() ? getInt16Ty() : getInt32Ty();
  Value *exp = scalarize(value, [this, expTy](Value *value) {
    return CreateIntrinsic(Intrinsic::amdgcn_frexp_exp, {expTy, value->getType()}, value);
  });
  exp->setName(instName);
  return exp;
}

// =====================================================================================================================
// Create vector cross product operation. Inputs must be <3 x FP>
//
// @param x : Input value X
// @param y : Input value Y
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateCrossProduct(Value *x, Value *y, const Twine &instName) {
  assert(x->getType() == y->getType() && cast<FixedVectorType>(x->getType())->getNumElements() == 3);

  Value *left = PoisonValue::get(x->getType());
  Value *right = PoisonValue::get(x->getType());
  for (unsigned idx = 0; idx != 3; ++idx) {
    left = CreateInsertElement(
        left, CreateFMul(CreateExtractElement(x, (idx + 1) % 3), CreateExtractElement(y, (idx + 2) % 3)), idx);
    right = CreateInsertElement(
        right, CreateFMul(CreateExtractElement(x, (idx + 2) % 3), CreateExtractElement(y, (idx + 1) % 3)), idx);
  }
  return CreateFSub(left, right, instName);
}

// =====================================================================================================================
// Create FP scalar/vector normalize operation: returns a scalar/vector with the same direction and magnitude 1.
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateNormalizeVector(Value *x, const Twine &instName) {
  if (!isa<VectorType>(x->getType())) {
    // For a scalar, just return -1.0 or +1.0.
    Value *isPositive = CreateFCmpOGT(x, Constant::getNullValue(x->getType()));
    return CreateSelect(isPositive, ConstantFP::get(x->getType(), 1.0), ConstantFP::get(x->getType(), -1.0), instName);
  }

  // For a vector, divide by the length.
  Value *dot = CreateDotProduct(x, x);
  Value *sqrt = CreateSqrt(dot);
  Value *rsq = CreateFDiv(ConstantFP::get(sqrt->getType(), 1.0), sqrt);
  Value *result = nullptr;
  if (x->getType()->getScalarType()->isFloatTy()) {
    // Make sure a FP32 zero vector is normalized to a FP32 zero vector, rather than NaNs.
    if (!getFastMathFlags().noSignedZeros() || !getFastMathFlags().noInfs() || !getFastMathFlags().noNaNs()) {
      // When NSZ, NoInfs, or NoNaNs is not specified, we avoid using fmul_legacy since it is not IEEE compliant.
      auto zero = ConstantFP::get(getFloatTy(), 0.0);
      auto isZeroDot = CreateFCmpOEQ(dot, zero);
      rsq = CreateSelect(isZeroDot, zero, rsq);
      result = scalarize(x, [this, rsq](Value *x) -> Value * { return CreateFMul(x, rsq); });
    } else {
      result = scalarize(x, [this, rsq](Value *x) -> Value * {
        return CreateIntrinsic(Intrinsic::amdgcn_fmul_legacy, {}, {x, rsq});
      });
    }
  } else {
    result = scalarize(x, [this, rsq](Value *x) -> Value * { return CreateFMul(x, rsq); });
  }
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "face forward" operation: given three FP scalars/vectors {N, I, Nref}, if the dot product of
// Nref and I is negative, the result is N, otherwise it is -N
//
// @param n : Input value "N"
// @param i : Input value "I"
// @param nref : Input value "Nref"
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFaceForward(Value *n, Value *i, Value *nref, const Twine &instName) {
  Value *dot = CreateDotProduct(i, nref);
  Value *isDotNegative = CreateFCmpOLT(dot, Constant::getNullValue(dot->getType()));
  Value *negN = CreateFSub(Constant::getNullValue(n->getType()), n);
  return CreateSelect(isDotNegative, n, negN, instName);
}

// =====================================================================================================================
// Create "reflect" operation. For the incident vector I and normalized surface orientation N, the result is
// the reflection direction:
// I - 2 * dot(N, I) * N
//
// @param i : Input value "I"
// @param n : Input value "N"
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateReflect(Value *i, Value *n, const Twine &instName) {
  Value *dot = CreateDotProduct(n, i);
  dot = CreateFMul(dot, ConstantFP::get(dot->getType(), 2.0));
  if (auto vecTy = dyn_cast<FixedVectorType>(n->getType()))
    dot = CreateVectorSplat(vecTy->getNumElements(), dot);
  return CreateFSub(i, CreateFMul(dot, n), instName);
}

// =====================================================================================================================
// Create "refract" operation. For the normalized incident vector I, normalized surface orientation N and ratio
// of indices of refraction eta, the result is the refraction vector:
// k = 1.0 - eta * eta * (1.0 - dot(N,I) * dot(N,I))
// If k < 0.0 the result is 0.0.
// Otherwise, the result is eta * I - (eta * dot(N,I) + sqrt(k)) * N
//
// @param i : Input value "I"
// @param n : Input value "N"
// @param eta : Input value "eta"
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateRefract(Value *i, Value *n, Value *eta, const Twine &instName) {
  Constant *one = ConstantFP::get(eta->getType(), 1.0);
  Value *dot = CreateDotProduct(i, n);
  Value *dotSqr = CreateFMul(dot, dot);
  Value *e1 = CreateFSub(one, dotSqr);
  Value *e2 = CreateFMul(eta, eta);
  Value *e3 = CreateFMul(e1, e2);
  Value *k = CreateFSub(one, e3);
  Value *kSqrt = CreateSqrt(k);
  Value *etaDot = CreateFMul(eta, dot);
  Value *innt = CreateFAdd(etaDot, kSqrt);

  if (auto vecTy = dyn_cast<FixedVectorType>(n->getType())) {
    eta = CreateVectorSplat(vecTy->getNumElements(), eta);
    innt = CreateVectorSplat(vecTy->getNumElements(), innt);
  }
  i = CreateFMul(i, eta);
  n = CreateFMul(n, innt);
  Value *s = CreateFSub(i, n);
  Value *con = CreateFCmpOLT(k, Constant::getNullValue(k->getType()));
  return CreateSelect(con, Constant::getNullValue(s->getType()), s);
}

// =====================================================================================================================
// Create "fclamp" operation, returning min(max(x, minVal), maxVal). Result is undefined if minVal > maxVal.
// This honors the fast math flags; clear "nnan" in fast math flags in order to obtain the "NaN avoiding
// semantics" for the min and max where, if one input is NaN, it returns the other one.
// It also honors the shader's FP mode being "flush denorm".
//
// @param x : Value to clamp
// @param minVal : Minimum of clamp range
// @param maxVal : Maximum of clamp range
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFClamp(Value *x, Value *minVal, Value *maxVal, const Twine &instName) {
  // For float, and for half on GFX9+, we can use the fmed3 instruction.
  // But we can only do this if we do not need NaN preservation.
  Value *result = nullptr;
  if (getFastMathFlags().noNaNs() && (x->getType()->getScalarType()->isFloatTy() ||
                                      (getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 9 &&
                                       x->getType()->getScalarType()->isHalfTy()))) {
    result = scalarize(x, minVal, maxVal, [this](Value *x, Value *minVal, Value *maxVal) {
      return CreateIntrinsic(Intrinsic::amdgcn_fmed3, x->getType(), {x, minVal, maxVal});
    });
    result->setName(instName);
  } else {
    // For half on GFX8 or earlier, or for double, use a combination of fmin and fmax.
    CallInst *max = CreateMaxNum(x, minVal);
    max->setFastMathFlags(getFastMathFlags());
    CallInst *min = CreateMinNum(max, maxVal, instName);
    min->setFastMathFlags(getFastMathFlags());
    result = min;
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "fmin" operation, returning the minimum of two scalar or vector FP values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
//
// @param value1 : First value
// @param value2 : Second value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFMin(Value *value1, Value *value2, const Twine &instName) {
  CallInst *min = CreateMinNum(value1, value2);
  min->setFastMathFlags(getFastMathFlags());
  Value *result = min;

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "fmax" operation, returning the maximum of two scalar or vector FP values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
//
// @param value1 : First value
// @param value2 : Second value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFMax(Value *value1, Value *value2, const Twine &instName) {
  CallInst *max = CreateMaxNum(value1, value2);
  max->setFastMathFlags(getFastMathFlags());
  Value *result = max;

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "fmin3" operation, returning the minimum of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
//
// @param value1 : First value
// @param value2 : Second value
// @param value3 : Third value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFMin3(Value *value1, Value *value2, Value *value3, const Twine &instName) {
  CallInst *min1 = CreateMinNum(value1, value2);
  min1->setFastMathFlags(getFastMathFlags());
  CallInst *min2 = CreateMinNum(min1, value3);
  min2->setFastMathFlags(getFastMathFlags());
  Value *result = min2;

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "fmax3" operation, returning the maximum of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
//
// @param value1 : First value
// @param value2 : Second value
// @param value3 : Third value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFMax3(Value *value1, Value *value2, Value *value3, const Twine &instName) {
  CallInst *max1 = CreateMaxNum(value1, value2);
  max1->setFastMathFlags(getFastMathFlags());
  CallInst *max2 = CreateMaxNum(max1, value3);
  max2->setFastMathFlags(getFastMathFlags());
  Value *result = max2;

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "fmid3" operation, returning the middle one of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
//
// @param value1 : First value
// @param value2 : Second value
// @param value3 : Third value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFMid3(Value *value1, Value *value2, Value *value3, const Twine &instName) {
  // For float, and for half on GFX9+, we can use the fmed3 instruction.
  // But we can only do this if we do not need NaN preservation.
  Value *result = nullptr;
  if (getFastMathFlags().noNaNs() && (value1->getType()->getScalarType()->isFloatTy() ||
                                      (getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 9 &&
                                       value1->getType()->getScalarType()->isHalfTy()))) {
    result = scalarize(value1, value2, value3, [this](Value *value1, Value *value2, Value *value3) {
      return CreateIntrinsic(Intrinsic::amdgcn_fmed3, value1->getType(), {value1, value2, value3});
    });
  } else {
    // For half on GFX8 or earlier, use a combination of fmin and fmax.
    CallInst *min1 = CreateMinNum(value1, value2);
    min1->setFastMathFlags(getFastMathFlags());
    CallInst *max1 = CreateMaxNum(value1, value2);
    max1->setFastMathFlags(getFastMathFlags());
    CallInst *min2 = CreateMinNum(max1, value3);
    min2->setFastMathFlags(getFastMathFlags());
    CallInst *max2 = CreateMaxNum(min1, min2, instName);
    max2->setFastMathFlags(getFastMathFlags());
    result = max2;
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Generate FP division, using fast fdiv for float to bypass optimization, and using fdiv 1.0 then fmul for
// half or double.
// TODO: IntrinsicsAMDGPU.td says amdgcn.fdiv.fast should not be used outside the backend.
//
// @param numerator : Numerator
// @param denominator : Denominator
Value *BuilderImpl::fDivFast(Value *numerator, Value *denominator) {
  if (!numerator->getType()->getScalarType()->isFloatTy())
    return CreateFMul(numerator, CreateFDiv(ConstantFP::get(denominator->getType(), 1.0), denominator));

  // We have to scalarize fdiv.fast ourselves.
  return scalarize(numerator, denominator, [this](Value *numerator, Value *denominator) -> Value * {
    return CreateIntrinsic(Intrinsic::amdgcn_fdiv_fast, {}, {numerator, denominator});
  });
}

// =====================================================================================================================
// Create "isInfinite" operation: return true if the supplied FP (or vector) value is infinity
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateIsInf(Value *x, const Twine &instName) {
  return createIsFPClass(x, CmpClass::NegativeInfinity | CmpClass::PositiveInfinity, instName);
}

// =====================================================================================================================
// Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateIsNaN(Value *x, const Twine &instName) {
  // 0x001: signaling NaN, 0x002: quiet NaN
  return createIsFPClass(x, CmpClass::SignalingNaN | CmpClass::QuietNaN, instName);
}

// =====================================================================================================================
// Helper method to create call to llvm.is.fpclass, scalarizing if necessary. This is not exposed outside of
// ArithBuilder.
//
// @param value : Input value
// @param flags : Flags for what class(es) to check for
// @param instName : Name to give instruction(s)
Value *BuilderImpl::createIsFPClass(Value *value, unsigned flags, const Twine &instName) {
  return CreateIntrinsic(Intrinsic::is_fpclass, value->getType(), {value, getInt32(flags)}, nullptr, instName);
}

// =====================================================================================================================
// Create an "insert bitfield" operation for a (vector of) integer type.
// Returns a value where the "count" bits starting at bit "offset" come from the least significant "count"
// bits in "insert", and remaining bits come from "base". The result is undefined if "count"+"offset" is
// more than the number of bits (per vector element) in "base" and "insert".
// If "base" and "insert" are vectors, "offset" and "count" can be either scalar or vector of the same
// width. The scalar type of "offset" and "count" must be integer, but can be different to that of "base"
// and "insert" (and different to each other too).
//
// @param base : Base value
// @param insert : Value to insert (same type as base)
// @param offset : Bit number of least-significant end of bitfield
// @param count : Count of bits in bitfield
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateInsertBitField(Value *base, Value *insert, Value *offset, Value *count,
                                         const Twine &instName) {
  // Make offset and count vectors of the right integer type if necessary.
  if (auto vecTy = dyn_cast<FixedVectorType>(base->getType())) {
    if (!isa<VectorType>(offset->getType()))
      offset = CreateVectorSplat(vecTy->getNumElements(), offset);
    if (!isa<VectorType>(count->getType()))
      count = CreateVectorSplat(vecTy->getNumElements(), count);
  }
  offset = CreateZExtOrTrunc(offset, base->getType());
  count = CreateZExtOrTrunc(count, base->getType());

  Constant *one = ConstantInt::get(count->getType(), 1);
  Value *mask = CreateShl(CreateSub(CreateShl(one, count), one), offset);
  Value *result = CreateOr(CreateAnd(CreateShl(insert, offset), mask), CreateAnd(base, CreateNot(mask)));
  Value *isWholeField = CreateICmpEQ(
      count, ConstantInt::get(count->getType(), count->getType()->getScalarType()->getPrimitiveSizeInBits()));
  return CreateSelect(isWholeField, insert, result, instName);
}

// =====================================================================================================================
// Create an "extract bitfield" operation for a (vector of) i32.
// Returns a value where the least significant "count" bits come from the "count" bits starting at bit
// "offset" in "base", and that is zero- or sign-extended (depending on "isSigned") to the rest of the value.
// If "base" and "insert" are vectors, "offset" and "count" can be either scalar or vector of the same
// width. The scalar type of "offset" and "count" must be integer, but can be different to that of "base"
// (and different to each other too).
//
// @param base : Base value
// @param offset : Bit number of least-significant end of bitfield
// @param count : Count of bits in bitfield
// @param isSigned : True for a signed int bitfield extract, false for unsigned
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateExtractBitField(Value *base, Value *offset, Value *count, bool isSigned,
                                          const Twine &instName) {
  // Make offset and count vectors of the right integer type if necessary.
  if (auto vecTy = dyn_cast<FixedVectorType>(base->getType())) {
    if (!isa<VectorType>(offset->getType()))
      offset = CreateVectorSplat(vecTy->getNumElements(), offset);
    if (!isa<VectorType>(count->getType()))
      count = CreateVectorSplat(vecTy->getNumElements(), count);
  }
  offset = CreateZExtOrTrunc(offset, base->getType());
  count = CreateZExtOrTrunc(count, base->getType());

  // For i32, we can use the amdgcn intrinsic and hence the instruction.
  if (base->getType()->getScalarType()->isIntegerTy(32)) {
    Value *isWholeField = CreateICmpEQ(
        count, ConstantInt::get(count->getType(), count->getType()->getScalarType()->getPrimitiveSizeInBits()));
    Value *result = scalarize(base, offset, count, [this, isSigned](Value *base, Value *offset, Value *count) {
      return CreateIntrinsic(isSigned ? Intrinsic::amdgcn_sbfe : Intrinsic::amdgcn_ubfe, base->getType(),
                             {base, offset, count});
    });
    result = CreateSelect(isWholeField, base, result);
    Value *isEmptyField = CreateICmpEQ(count, Constant::getNullValue(count->getType()));
    return CreateSelect(isEmptyField, Constant::getNullValue(count->getType()), result, instName);
  }

  // For other types, extract manually.
  Value *shiftDown =
      CreateSub(ConstantInt::get(base->getType(), base->getType()->getScalarType()->getPrimitiveSizeInBits()), count);
  Value *shiftUp = CreateSub(shiftDown, offset);
  Value *result = CreateShl(base, shiftUp);
  if (isSigned)
    result = CreateAShr(result, shiftDown);
  else
    result = CreateLShr(result, shiftDown);
  Value *isZeroCount = CreateICmpEQ(count, Constant::getNullValue(count->getType()));
  return CreateSelect(isZeroCount, count, result, instName);
}

// =====================================================================================================================
// Create "find MSB" operation for a (vector of) signed i32. For a positive number, the result is the bit number of
// the most significant 1-bit. For a negative number, the result is the bit number of the most significant 0-bit.
// For a value of 0 or -1, the result is -1.
//
// @param value : Input value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateFindSMsb(Value *value, const Twine &instName) {
  assert(value->getType()->getScalarType()->isIntegerTy(32));

  Constant *negOne = ConstantInt::get(value->getType(), -1);
  Value *leadingSignBitsCount = CreateCountLeadingSignBits(value);
  Value *isNegOne = CreateICmpEQ(leadingSignBitsCount, negOne);
  Value *bitOnePos = CreateSub(ConstantInt::get(value->getType(), 31), leadingSignBitsCount);
  return CreateSelect(isNegOne, negOne, bitOnePos, instName);
}

// =====================================================================================================================
// Create "count leading sign bits" operation for a (vector of) signed i32. For a positive number, the result is the
// count of the most leading significant 1-bit. For a negative number, the result is the bit number of the most
// significant 0-bit. For a value of 0 or -1, the result is -1.
//
// @param value : Input value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateCountLeadingSignBits(Value *value, const Twine &instName) {
  assert(value->getType()->getScalarType()->isIntegerTy(32));

  Value *result =
      scalarize(value, [this](Value *value) { return CreateUnaryIntrinsic(Intrinsic::amdgcn_sffbh, value); });
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "msad" (Masked Sum of Absolute Differences) operation , returning an 32-bit integer of msad result.
//
// @param src : Contains 4 packed 8-bit unsigned integers in 32 bits.
// @param ref : Contains 4 packed 8-bit unsigned integers in 32 bits.
// @param accum : A 32-bit unsigned integer, providing an existing accumulation.
Value *BuilderImpl::CreateMsad4(Value *src, Value *ref, Value *accum, const Twine &instName) {
  assert(ref->getType()->getScalarType()->isIntegerTy(32));

  Value *result = scalarize(src, ref, accum, [this](Value *src, Value *ref, Value *accum) {
    return CreateIntrinsic(src->getType(), Intrinsic::amdgcn_msad_u8, {src, ref, accum});
  });
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "fdot2" operation, returning a float result of the sum of dot2 of 2 half vec2 and a float scalar.
//
// @param a : Vector of 2xhalf A.
// @param b : Vector of 2xhalf B.
// @param scalar : A float scalar.
// @param clamp : Whether the accumulation result should be clamped.
Value *BuilderImpl::CreateFDot2(Value *a, Value *b, Value *scalar, Value *clamp, const Twine &instName) {
  assert(a->getType()->getScalarType()->isHalfTy() && b->getType()->getScalarType()->isHalfTy());
  assert(scalar->getType()->isFloatTy());
  assert(clamp->getType()->isIntegerTy() && clamp->getType()->getIntegerBitWidth() == 1);

  Value *result = CreateIntrinsic(scalar->getType(), Intrinsic::amdgcn_fdot2, {a, b, scalar, clamp});
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create "fmix" operation, returning ( 1 - A ) * X + A * Y. Result would be FP scalar or vector value.
// Returns scalar, if and only if "pX", "pY" and "pA" are all scalars.
// Returns vector, if "pX" and "pY" are vector but "pA" is a scalar, under such condition, "pA" will be splatted.
// Returns vector, if "pX", "pY" and "pA" are all vectors.
// Note that when doing vector calculation, it means add/sub are element-wise between vectors, and the product will
// be Hadamard product.
//
// @param x : left Value
// @param y : right Value
// @param a : wight Value
// @param instName : Name to give instruction(s)
Value *BuilderImpl::createFMix(Value *x, Value *y, Value *a, const Twine &instName) {
  Value *ySubX = CreateFSub(y, x);
  if (auto vectorResultTy = dyn_cast<FixedVectorType>(ySubX->getType())) {
    // x, y => vector, but a => scalar
    if (!isa<VectorType>(a->getType()))
      a = CreateVectorSplat(vectorResultTy->getNumElements(), a);
  }

  IRBuilderBase::FastMathFlagGuard FMFGuard(*this);
  getFastMathFlags().setNoNaNs();
  getFastMathFlags().setAllowContract();
  Value *fmul = CreateFMul(ySubX, a);
  Value *result = CreateFAdd(fmul, x, instName);

  return result;
}

// =====================================================================================================================
// Ensure result is canonicalized if the shader's FP mode is flush denorms. This is called on an FP result of an
// instruction that does not honor the hardware's FP mode, such as fmin/fmax/fmed on GFX8 and earlier.
//
// @param value : Value to canonicalize
Value *BuilderImpl::canonicalize(Value *value) {
  const auto &shaderMode = getShaderModes()->getCommonShaderMode(m_shaderStage);
  auto destTy = value->getType();
  FpDenormMode denormMode = destTy->getScalarType()->isHalfTy()     ? shaderMode.fp16DenormMode
                            : destTy->getScalarType()->isFloatTy()  ? shaderMode.fp32DenormMode
                            : destTy->getScalarType()->isDoubleTy() ? shaderMode.fp64DenormMode
                                                                    : FpDenormMode::DontCare;
  if (denormMode == FpDenormMode::FlushOut || denormMode == FpDenormMode::FlushInOut) {
    // Has to flush denormals, insert canonicalize to make a MUL (* 1.0) forcibly
    value = CreateUnaryIntrinsic(Intrinsic::canonicalize, value);
  }
  return value;
}
