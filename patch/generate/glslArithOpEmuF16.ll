;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 ;
 ;  Permission is hereby granted, free of charge, to any person obtaining a copy
 ;  of this software and associated documentation files (the "Software"), to deal
 ;  in the Software without restriction, including without limitation the rights
 ;  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 ;  copies of the Software, and to permit persons to whom the Software is
 ;  furnished to do so, subject to the following conditions:
 ;
 ;  The above copyright notice and this permission notice shall be included in all
 ;  copies or substantial portions of the Software.
 ;
 ;  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ;  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ;  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 ;  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 ;  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 ;  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 ;  SOFTWARE.
 ;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

target datalayout = "e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5"

; =====================================================================================================================
; >>>  Operators
; =====================================================================================================================

; GLSL: float16_t = float16_t(float) (rounding mode: RTZ)
define spir_func half @_Z16convert_half_rtzf(float %x) #0
{
    %1 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %x, float 0.0) #1
    %2 = extractelement <2 x half> %1, i32 0
    ret half %2
}

; GLSL: f16vec2 = f16vec2(vec2) (rounding mode: RTZ)
define spir_func <2 x half> @_Z17convert_half2_rtzDv2_f(<2 x float> %x) #0
{
    %1 = extractelement <2 x float> %x, i32 0
    %2 = extractelement <2 x float> %x, i32 1
    %3 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %1, float %2) #1
    ret <2 x half> %3
}

; GLSL: f16vec3 = f16vec3(vec3) (rounding mode: RTZ)
define spir_func <3 x half> @_Z17convert_half3_rtzDv3_f(<3 x float> %x) #0
{
    %1 = extractelement <3 x float> %x, i32 0
    %2 = extractelement <3 x float> %x, i32 1
    %3 = extractelement <3 x float> %x, i32 2
    %4 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %1, float %2) #1
    %5 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %3, float 0.0) #1
    %6 = shufflevector <2 x half> %4, <2 x half> %5, <3 x i32> <i32 0, i32 1, i32 2>
    ret <3 x half> %6
}

; GLSL: f16vec4 = f16vec4(vec4) (rounding mode: RTZ)
define spir_func <4 x half> @_Z17convert_half4_rtzDv4_f(<4 x float> %x) #0
{
    %1 = extractelement <4 x float> %x, i32 0
    %2 = extractelement <4 x float> %x, i32 1
    %3 = extractelement <4 x float> %x, i32 2
    %4 = extractelement <4 x float> %x, i32 3
    %5 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %1, float %2) #1
    %6 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %3, float %4) #1
    %7 = shufflevector <2 x half> %5, <2 x half> %6, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
    ret <4 x half> %7
}

; Rounds the given bits to nearest even by discarding the last numBitsToDiscard bits.
define i32 @llpc.round.tonearest.f32(i32 %bits, i32 %numBitsToDiscard)
{
.entry:
    ; lastBits = bits & ((1 << numBitsToDiscard) - 1)
    %lastBits.1 = shl i32 1, %numBitsToDiscard
    %lastBits.2 = sub i32 %lastBits.1, 1
    %lastBits = and i32 %bits, %lastBits.2

    ; headBit = bits & (1 << (numBitsToDiscard - 1))
    %headBit.1 = sub i32 %numBitsToDiscard, 1
    %headBit.2 = shl i32 1, %headBit.1
    %headBit = and i32 %bits, %headBit.2

    ; bits = (bits >> numBitsToDiscard)
    %bits.1 = lshr i32 %bits, %numBitsToDiscard

    ; bits + 1
    %bits.2 = add i32 %bits.1, 1

    ;
    ; if (headBit == 0)
    ;   return bits
    ;
    ; else if (headBit == lastBits)
    ;   return ((bits & 0x1) == 0x1) ? bits + 1 : bits
    ;
    ; else
    ;   return bits + 1
    ;

    ; (headBits == lastBits) ?
    %isHeadLastEqual = icmp eq i32 %headBit, %lastBits

    ; ((bits & 0x1) == 0x1) ? bits + 1 : bits
    %bits.3 = and i32 %bits.1, 1
    %isOne = icmp eq i32 %bits.3, 1
    %bits.4 = select i1 %isOne, i32 %bits.2, i32 %bits.1

    %bits.5 = select i1 %isHeadLastEqual, i32 %bits.4, i32 %bits.2

    ; (headBit == 0) ?
    %isZeroHeadBit = icmp eq i32 %headBit, 0

    %roundBits = select i1 %isZeroHeadBit, i32 %bits.1, i32 %bits.5

    ret i32 %roundBits
}

; GLSL: float16_t = float16_t(float) (rounding mode: RTE)
define spir_func half @_Z16convert_half_rtef(float %x) #0
{
.entry:
    ; float32: sign = [31], exponent = [30:23], mantissa = [22:0]
    ; float16: sign = [15], exponent = [14:10], mantissa = [9:0]

    %bits32 = bitcast float %x to i32

    ; sign = (bits32 >> 16) & 0x8000
    %sign.1 = lshr i32 %bits32, 16
    %sign = and i32 %sign.1, 32768

    ; exp32 = (bits32 >> 23) & 0xFF
    %exp32.1 = lshr i32 %bits32, 23
    %exp32 = and i32 %exp32.1, 255

    ; exp16 = exp32 - 127 + 15
    %exp16 = sub i32 %exp32, 112

    ; mant = bits32 & 0x7FFFFF
    %mant = and i32 %bits32, 8388607

    ; (exp32 == 0) ?
    %isZero = icmp eq i32 %exp32, 0
    br i1 %isZero, label %.toZero, label %.checkNaNInf

.toZero:
    ; sign
    br label %.end

.checkNaNInf:
    ; (exp32 == 255) ?
    %isNaNInf = icmp eq i32 %exp32, 255
    br i1 %isNaNInf, label %.checkInf, label %.checkTooSmall

.checkInf:
    ; (mant == 0) ?
    %isInf = icmp eq i32 %mant, 0
    br i1 %isInf, label %.toInf, label %.toNaN

.toInf:
    ; (sign | 0x7C00)
    %inf16 = or i32 %sign, 31744
    br label %.end

.toNaN:
    ; mant = (mant >> 13)
    ; (sign | 0x7C00 | mant | 0x1)
    %mant.1 = lshr i32 %mant, 13
    %nan16.1 = or i32 %sign, 31745
    %nan16 = or i32 %nan16.1, %mant.1
    br label %.end

.checkTooSmall:
    ; (exp16 < -10)
    %isTooSmall = icmp slt i32 %exp16, -10
    br i1 %isTooSmall, label %.toZero, label %.checkDenorm

.checkDenorm:
    ; (exp16 <= 0)
    %isDenorm = icmp sle i32 %exp16, 0
    br i1 %isDenorm, label %.toDenorm, label %.checkNorm

.toDenorm:
    ; mant = mant | 0x800000
    %mant.2 = or i32 %mant, 8388608

    ; numBitsToDiscard = 14 - exp16
    %numBitsToDiscard.1 = sub i32 14, %exp16

    ; mant = RoundToNearestEven(mant, 14 - exp16)
    %mant.3 = call i32 @llpc.round.tonearest.f32(i32 %mant.2, i32 %numBitsToDiscard.1)

    ; (sign | mant)
    %denorm16 = or i32 %sign, %mant.3
    br label %.end

.checkNorm:
    ; (exp16 <= 30)
    %isNorm = icmp sle i32 %exp16, 30
    br i1 %isNorm, label %.toNorm, label %.toOverflow

.toNorm:
    ; mant = RoundToNearestEven(mant, 13)
    %mant.4 = call i32 @llpc.round.tonearest.f32(i32 %mant, i32 13)

    ; exp16 = (exp16 << 10) + (mant & (1 << 10))
    %exp16.1 = shl i32 %exp16, 10
    %mant.5 = and i32 %mant.4, 1024
    %exp16.2 = add i32 %exp16.1, %mant.5

    ; mant &= (1 << 10) - 1
    %mant.6 = and i32 %mant.4, 1023

    ; (sign | exp16 | mant)
    %norm16.1 = or i32 %sign, %exp16.2
    %norm16 = or i32 %norm16.1, %mant.6
    br label %.end

.toOverflow:
    ; (sign | (0x1F << 10))
    %tooLarge16 = or i32 %sign, 31744
    br label %.end

.end:
    %bits16.1 = phi i32 [ %sign, %.toZero ],
                        [ %inf16, %.toInf ],
                        [ %nan16, %.toNaN ],
                        [ %denorm16, %.toDenorm ],
                        [ %norm16, %.toNorm ],
                        [ %tooLarge16, %.toOverflow]

    %bits16 = trunc i32 %bits16.1 to i16
    %f16 = bitcast i16 %bits16 to half

    ret half %f16
}

; GLSL: f16vec2 = f16vec2(vec2) (rounding mode: RTE)
define spir_func <2 x half> @_Z17convert_half2_rteDv2_f(<2 x float> %x) #0
{
    %1 = extractelement <2 x float> %x, i32 0
    %2 = extractelement <2 x float> %x, i32 1

    %3 = call half @_Z16convert_half_rtef(float %1)
    %4 = call half @_Z16convert_half_rtef(float %2)

    %5 = insertelement <2 x half> undef, half %3, i32 0
    %6 = insertelement <2 x half> %5, half %4, i32 1

    ret <2 x half> %6
}

; GLSL: f16vec3 = f16vec3(vec3) (rounding mode: RTE)
define spir_func <3 x half> @_Z17convert_half3_rteDv3_f(<3 x float> %x) #0
{
    %1 = extractelement <3 x float> %x, i32 0
    %2 = extractelement <3 x float> %x, i32 1
    %3 = extractelement <3 x float> %x, i32 2

    %4 = call half @_Z16convert_half_rtef(float %1)
    %5 = call half @_Z16convert_half_rtef(float %2)
    %6 = call half @_Z16convert_half_rtef(float %3)

    %7 = insertelement <3 x half> undef, half %4, i32 0
    %8 = insertelement <3 x half> %7, half %5, i32 1
    %9 = insertelement <3 x half> %8, half %6, i32 2

    ret <3 x half> %9
}

; GLSL: f16vec4 = f16vec4(vec4) (rounding mode: RTE)
define spir_func <4 x half> @_Z17convert_half4_rteDv4_f(<4 x float> %x) #0
{
    %1 = extractelement <4 x float> %x, i32 0
    %2 = extractelement <4 x float> %x, i32 1
    %3 = extractelement <4 x float> %x, i32 2
    %4 = extractelement <4 x float> %x, i32 3

    %5 = call half @_Z16convert_half_rtef(float %1)
    %6 = call half @_Z16convert_half_rtef(float %2)
    %7 = call half @_Z16convert_half_rtef(float %3)
    %8 = call half @_Z16convert_half_rtef(float %4)

    %9 = insertelement <4 x half> undef, half %5, i32 0
    %10 = insertelement <4 x half> %9, half %6, i32 1
    %11 = insertelement <4 x half> %10, half %7, i32 2
    %12 = insertelement <4 x half> %11, half %8, i32 3

    ret <4 x half> %12
}

; GLSL: float16_t = float16_t(float) (rounding mode: RTP)
define spir_func half @_Z16convert_half_rtpf(float %x) #0
{
    ; TODO: Use s.setreg() to change HW_REG_MODE.
    %1 = fptrunc float %x to half
    ret half %1
}

; GLSL: f16vec2 = f16vec2(vec2) (rounding mode: RTP)
define spir_func <2 x half> @_Z17convert_half2_rtpDv2_f(<2 x float> %x) #0
{
    ; TODO: Use s.setreg() to change HW_REG_MODE.
    %1 = fptrunc <2 x float> %x to <2 x half>
    ret <2 x half> %1
}

; GLSL: f16vec3 = f16vec3(vec3) (rounding mode: RTP)
define spir_func <3 x half> @_Z17convert_half3_rtpDv3_f(<3 x float> %x) #0
{
    ; TODO: Use s.setreg() to change HW_REG_MODE.
    %1 = fptrunc <3 x float> %x to <3 x half>
    ret <3 x half> %1
}

; GLSL: f16vec4 = f16vec4(vec4) (rounding mode: RTP)
define spir_func <4 x half> @_Z17convert_half4_rtpDv4_f(<4 x float> %x) #0
{
    ; TODO: Use s.setreg() to change HW_REG_MODE.
    %1 = fptrunc <4 x float> %x to <4 x half>
    ret <4 x half> %1
}

; GLSL: float16_t = float16_t(float) (rounding mode: RTN)
define spir_func half @_Z16convert_half_rtnf(float %x) #0
{
    ; TODO: Use s.setreg() to change HW_REG_MODE.
    %1 = fptrunc float %x to half
    ret half %1
}

; GLSL: f16vec2 = f16vec2(vec2) (rounding mode: RTN)
define spir_func <2 x half> @_Z17convert_half2_rtnDv2_f(<2 x float> %x) #0
{
    ; TODO: Use s.setreg() to change HW_REG_MODE.
    %1 = fptrunc <2 x float> %x to <2 x half>
    ret <2 x half> %1
}

; GLSL: f16vec3 = f16vec3(vec3) (rounding mode: RTN)
define spir_func <3 x half> @_Z17convert_half3_rtnDv3_f(<3 x float> %x) #0
{
    ; TODO: Use s.setreg() to change HW_REG_MODE.
    %1 = fptrunc <3 x float> %x to <3 x half>
    ret <3 x half> %1
}

; GLSL: f16vec4 = f16vec4(vec4) (rounding mode: RTN)
define spir_func <4 x half> @_Z17convert_half4_rtnDv4_f(<4 x float> %x) #0
{
    ; TODO: Use s.setreg() to change HW_REG_MODE.
    %1 = fptrunc <4 x float> %x to <4 x half>
    ret <4 x half> %1
}

; GLSL: float16_t = float16_t(double) (rounding mode: RTZ)
define spir_func half @_Z16convert_half_rtzd(double %x) #0
{
    %1 = fptrunc double %x to float
    %2 = call half @_Z16convert_half_rtzf(float %1)

    ret half %2
}

; GLSL: f16vec2 = f16vec2(dvec2) (rounding mode: RTZ)
define spir_func <2 x half> @_Z17convert_half2_rtzDv2_d(<2 x double> %x) #0
{
    %1 = fptrunc <2 x double> %x to <2 x float>
    %2 = call <2 x half> @_Z17convert_half2_rtzDv2_f(<2 x float> %1)

    ret <2 x half> %2
}

; GLSL: f16vec3 = f16vec3(dvec3) (rounding mode: RTZ)
define spir_func <3 x half> @_Z17convert_half3_rtzDv3_d(<3 x double> %x) #0
{
    %1 = fptrunc <3 x double> %x to <3 x float>
    %2 = call <3 x half> @_Z17convert_half3_rtzDv3_f(<3 x float> %1)

    ret <3 x half> %2
}

; GLSL: f16vec4 = f16vec4(dvec4) (rounding mode: RTZ)
define spir_func <4 x half> @_Z17convert_half4_rtzDv4_d(<4 x double> %x) #0
{
    %1 = fptrunc <4 x double> %x to <4 x float>
    %2 = call <4 x half> @_Z17convert_half4_rtzDv4_f(<4 x float> %1)

    ret <4 x half> %2
}

; GLSL: float16_t = float16_t(double) (rounding mode: RTE)
define spir_func half @_Z16convert_half_rted(double %x) #0
{
    %1 = fptrunc double %x to float
    %2 = call half @_Z16convert_half_rtef(float %1)

    ret half %2
}

; GLSL: f16vec2 = f16vec2(dvec2) (rounding mode: RTE)
define spir_func <2 x half> @_Z17convert_half2_rteDv2_d(<2 x double> %x) #0
{
    %1 = fptrunc <2 x double> %x to <2 x float>
    %2 = call <2 x half> @_Z17convert_half2_rteDv2_f(<2 x float> %1)

    ret <2 x half> %2
}

; GLSL: f16vec3 = f16vec3(dvec3) (rounding mode: RTE)
define spir_func <3 x half> @_Z17convert_half3_rteDv3_d(<3 x double> %x) #0
{
    %1 = fptrunc <3 x double> %x to <3 x float>
    %2 = call <3 x half> @_Z17convert_half3_rteDv3_f(<3 x float> %1)

    ret <3 x half> %2
}

; GLSL: f16vec4 = f16vec4(dvec4) (rounding mode: RTE)
define spir_func <4 x half> @_Z17convert_half4_rteDv4_d(<4 x double> %x) #0
{
    %1 = fptrunc <4 x double> %x to <4 x float>
    %2 = call <4 x half> @_Z17convert_half4_rteDv4_f(<4 x float> %1)

    ret <4 x half> %2
}

; =====================================================================================================================
; >>>  Common Functions
; =====================================================================================================================

; GLSL: float16_t mod(float16_t, float16_t)
define half @llpc.mod.f16(half %x, half %y) #0
{
    %1 = fdiv half 1.0,%y
    %2 = fmul half %x, %1
    %3 = call half @llvm.floor.f16(half %2)
    %4 = fmul half %y, %3
    %5 = fsub half %x, %4
    ret half %5
}

; GLSL: bool isinf(float16_t)
define i1 @llpc.isinf.f16(half %x) #0
{
    ; 0x004: negative infinity; 0x200: positive infinity
    %1 = call i1 @llvm.amdgcn.class.f16(half %x, i32 516)
    ret i1 %1
}

; GLSL: bool isnan(float16_t)
define i1 @llpc.isnan.f16(half %x) #0
{
    ; 0x001: signaling NaN, 0x002: quiet NaN
    %1 = call i1 @llvm.amdgcn.class.f16(half %x, i32 3)
    ret i1 %1
}

declare half @llvm.trunc.f16(half) #0
declare half @llvm.fabs.f16(half) #0
declare half @llvm.sqrt.f16(half) #0
declare half @llvm.floor.f16(half) #0
declare half @llvm.exp2.f16(half) #0
declare half @llvm.log2.f16(half) #0
declare half @llvm.sin.f16(half) #0
declare half @llvm.cos.f16(half) #0
declare float @llpc.asin.f32(float) #0
declare float @llpc.acos.f32(float) #0
declare half @llvm.minnum.f16(half, half) #0
declare half @llvm.maxnum.f16(half, half) #0
declare half @llvm.fmuladd.f16(half, half, half) #0
declare i1 @llvm.amdgcn.class.f16(half, i32) #1
declare half @llvm.amdgcn.fract.f16(half) #1
declare half @llvm.amdgcn.fmed3.f16(half, half, half) #1
declare half @llvm.rint.f16(half) #0
declare i16 @llvm.amdgcn.frexp.exp.i16.f16(half) #1
declare half @llvm.amdgcn.frexp.mant.f16(half) #1
declare <2 x half> @llvm.amdgcn.cvt.pkrtz(float, float) #1
declare i32 @llvm.amdgcn.ubfe.i32(i32, i32, i32) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
