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

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024"
target triple = "spir64-unknown-unknown"

; =====================================================================================================================
; >>>  Common Functions
; =====================================================================================================================

; GLSL: float mod(float, float)
define float @llpc.mod.f32(float %x, float %y) #0
{
    %1 = fdiv float 1.0,%y
    %2 = fmul float %x, %1
    %3 = call float @llvm.floor.f32(float %2)
    %4 = fmul float %y, %3
    %5 = fsub float %x, %4
    ret float %5
}

; GLSL: bool isinf(float)
define i1 @llpc.isinf.f32(float %x) #0
{
    ; 0x004: negative infinity; 0x200: positive infinity
    %1 = call i1 @llvm.amdgcn.class.f32(float %x, i32 516)
    ret i1 %1
}

; GLSL: bool isnan(float)
define i1 @llpc.isnan.f32(float %x) #0
{
    ; 0x001: signaling NaN, 0x002: quiet NaN
    %1 = call i1 @llvm.amdgcn.class.f32(float %x, i32 3)
    ret i1 %1
}

; =====================================================================================================================
; >>>  Vector Relational Functions
; =====================================================================================================================

; GLSL: bool = any(bool)
define spir_func i32 @_Z3anyi(
    i32 %x) #0
{
    ret i32 %x
}

; GLSL: bool = any(bvec2)
define spir_func i32 @_Z3anyDv2_i(
    <2 x i32> %x) #0
{
    %x0 = extractelement <2 x i32> %x, i32 0
    %x1 = extractelement <2 x i32> %x, i32 1

    %1 = or i32 %x1, %x0

    ret i32 %1
}

; GLSL: bool = any(bvec3)
define spir_func i32 @_Z3anyDv3_i(
    <3 x i32> %x) #0
{
    %x0 = extractelement <3 x i32> %x, i32 0
    %x1 = extractelement <3 x i32> %x, i32 1
    %x2 = extractelement <3 x i32> %x, i32 2

    %1 = or i32 %x1, %x0
    %2 = or i32 %x2, %1

    ret i32 %2
}

; GLSL: bool = any(bvec4)
define spir_func i32 @_Z3anyDv4_i(
    <4 x i32> %x) #0
{
    %x0 = extractelement <4 x i32> %x, i32 0
    %x1 = extractelement <4 x i32> %x, i32 1
    %x2 = extractelement <4 x i32> %x, i32 2
    %x3 = extractelement <4 x i32> %x, i32 3

    %1 = or i32 %x1, %x0
    %2 = or i32 %x2, %1
    %3 = or i32 %x3, %2

    ret i32 %3
}

; GLSL: bool = all(bool)
define spir_func i32 @_Z3alli(
    i32 %x) #0
{
    ret i32 %x
}

; GLSL: bool = all(bvec2)
define spir_func i32 @_Z3allDv2_i(
    <2 x i32> %x) #0
{
    %x0 = extractelement <2 x i32> %x, i32 0
    %x1 = extractelement <2 x i32> %x, i32 1

    %1 = and i32 %x1, %x0

    ret i32 %1
}

; GLSL: bool = all(bvec3)
define spir_func i32 @_Z3allDv3_i(
    <3 x i32> %x) #0
{
    %x0 = extractelement <3 x i32> %x, i32 0
    %x1 = extractelement <3 x i32> %x, i32 1
    %x2 = extractelement <3 x i32> %x, i32 2

    %1 = and i32 %x1, %x0
    %2 = and i32 %x2, %1

    ret i32 %2
}

; GLSL: bool = all(bvec4)
define spir_func i32 @_Z3allDv4_i(
    <4 x i32> %x) #0
{
    %x0 = extractelement <4 x i32> %x, i32 0
    %x1 = extractelement <4 x i32> %x, i32 1
    %x2 = extractelement <4 x i32> %x, i32 2
    %x3 = extractelement <4 x i32> %x, i32 3

    %1 = and i32 %x1, %x0
    %2 = and i32 %x2, %1
    %3 = and i32 %x3, %2

    ret i32 %3
}

; GLSL: uint uaddCarry(uint, uint, out uint)
define spir_func {i32, i32} @_Z9IAddCarryii(
    i32 %x, i32 %y) #0
{
    %1 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x, i32 %y)
    %2 = extractvalue { i32, i1 } %1, 0
    %3 = extractvalue { i32, i1 } %1, 1
    %4 = zext i1 %3 to i32

    %5 = insertvalue { i32, i32 } undef, i32 %2, 0
    %6 = insertvalue { i32, i32 } %5, i32 %4, 1

    ret {i32, i32} %6
}

; GLSL: uvec2 uaddCarry(uvec2, uvec2, out uvec2)
define spir_func {<2 x i32>, <2 x i32>} @_Z9IAddCarryDv2_iDv2_i(
    <2 x i32> %x, <2 x i32> %y) #0
{
    %x0 = extractelement <2 x i32> %x, i32 0
    %x1 = extractelement <2 x i32> %x, i32 1

    %y0 = extractelement <2 x i32> %y, i32 0
    %y1 = extractelement <2 x i32> %y, i32 1

    %1 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x0, i32 %y0)
    %2 = extractvalue { i32, i1 } %1, 0
    %3 = extractvalue { i32, i1 } %1, 1
    %4 = zext i1 %3 to i32

    %5 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x1, i32 %y1)
    %6 = extractvalue { i32, i1 } %5, 0
    %7 = extractvalue { i32, i1 } %5, 1
    %8 = zext i1 %7 to i32

    %9 = insertelement <2 x i32> undef, i32 %2, i32 0
    %10 = insertelement <2 x i32> %9, i32 %6, i32 1

    %11 = insertelement <2 x i32> undef, i32 %4, i32 0
    %12 = insertelement <2 x i32> %11, i32 %8, i32 1

    %13 = insertvalue { <2 x i32>, <2 x i32> } undef, <2 x i32> %10, 0
    %14 = insertvalue { <2 x i32>, <2 x i32> } %13, <2 x i32> %12, 1

    ret {<2 x i32>, <2 x i32>} %14
}

; GLSL: uvec3 uaddCarry(uvec3, uvec3, out uvec3)
define spir_func {<3 x i32>, <3 x i32>} @_Z9IAddCarryDv3_iDv3_i(
    <3 x i32> %x, <3 x i32> %y) #0
{
    %x0 = extractelement <3 x i32> %x, i32 0
    %x1 = extractelement <3 x i32> %x, i32 1
    %x2 = extractelement <3 x i32> %x, i32 2

    %y0 = extractelement <3 x i32> %y, i32 0
    %y1 = extractelement <3 x i32> %y, i32 1
    %y2 = extractelement <3 x i32> %y, i32 2

    %1 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x0, i32 %y0)
    %2 = extractvalue { i32, i1 } %1, 0
    %3 = extractvalue { i32, i1 } %1, 1
    %4 = zext i1 %3 to i32

    %5 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x1, i32 %y1)
    %6 = extractvalue { i32, i1 } %5, 0
    %7 = extractvalue { i32, i1 } %5, 1
    %8 = zext i1 %7 to i32

    %9 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x2, i32 %y2)
    %10 = extractvalue { i32, i1 } %9, 0
    %11 = extractvalue { i32, i1 } %9, 1
    %12 = zext i1 %11 to i32

    %13 = insertelement <3 x i32> undef, i32 %2, i32 0
    %14 = insertelement <3 x i32> %13, i32 %6, i32 1
    %15 = insertelement <3 x i32> %14, i32 %10, i32 2

    %16 = insertelement <3 x i32> undef, i32 %4, i32 0
    %17 = insertelement <3 x i32> %16, i32 %8, i32 1
    %18 = insertelement <3 x i32> %17, i32 %12, i32 2

    %19 = insertvalue { <3 x i32>, <3 x i32> } undef, <3 x i32> %15, 0
    %20 = insertvalue { <3 x i32>, <3 x i32> } %19, <3 x i32> %18, 1

    ret {<3 x i32>, <3 x i32>} %20
}

; GLSL: uvec4 uaddCarry(uvec4, uvec4, out uvec4)
define spir_func {<4 x i32>, <4 x i32>} @_Z9IAddCarryDv4_iDv4_i(
    <4 x i32> %x, <4 x i32> %y) #0
{
    %x0 = extractelement <4 x i32> %x, i32 0
    %x1 = extractelement <4 x i32> %x, i32 1
    %x2 = extractelement <4 x i32> %x, i32 2
    %x3 = extractelement <4 x i32> %x, i32 3

    %y0 = extractelement <4 x i32> %y, i32 0
    %y1 = extractelement <4 x i32> %y, i32 1
    %y2 = extractelement <4 x i32> %y, i32 2
    %y3 = extractelement <4 x i32> %y, i32 3

    %1 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x0, i32 %y0)
    %2 = extractvalue { i32, i1 } %1, 0
    %3 = extractvalue { i32, i1 } %1, 1
    %4 = zext i1 %3 to i32

    %5 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x1, i32 %y1)
    %6 = extractvalue { i32, i1 } %5, 0
    %7 = extractvalue { i32, i1 } %5, 1
    %8 = zext i1 %7 to i32

    %9 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x2, i32 %y2)
    %10 = extractvalue { i32, i1 } %9, 0
    %11 = extractvalue { i32, i1 } %9, 1
    %12 = zext i1 %11 to i32

    %13 = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x3, i32 %y3)
    %14 = extractvalue { i32, i1 } %13, 0
    %15 = extractvalue { i32, i1 } %13, 1
    %16 = zext i1 %15 to i32

    %17 = insertelement <4 x i32> undef, i32 %2, i32 0
    %18 = insertelement <4 x i32> %17, i32 %6, i32 1
    %19 = insertelement <4 x i32> %18, i32 %10, i32 2
    %20 = insertelement <4 x i32> %19, i32 %14, i32 3

    %21 = insertelement <4 x i32> undef, i32 %4, i32 0
    %22 = insertelement <4 x i32> %21, i32 %8, i32 1
    %23 = insertelement <4 x i32> %22, i32 %12, i32 2
    %24 = insertelement <4 x i32> %23, i32 %16, i32 3

    %25 = insertvalue { <4 x i32>, <4 x i32> } undef, <4 x i32> %20, 0
    %26 = insertvalue { <4 x i32>, <4 x i32> } %25, <4 x i32> %24, 1

    ret {<4 x i32>, <4 x i32>} %26
}

; GLSL: uint usubBorrow(uint, uint, out uint)
define spir_func {i32, i32} @_Z10ISubBorrowii(
    i32 %x, i32 %y) #0
{
    %1 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x, i32 %y)
    %2 = extractvalue { i32, i1 } %1, 0
    %3 = extractvalue { i32, i1 } %1, 1
    %4 = zext i1 %3 to i32

    %5 = insertvalue { i32, i32 } undef, i32 %2, 0
    %6 = insertvalue { i32, i32 } %5, i32 %4, 1

    ret {i32, i32} %6
}

; GLSL: uvec2 usubBorrow(uvec2, uvec2, out uvec2)
define spir_func {<2 x i32>, <2 x i32>} @_Z10ISubBorrowDv2_iDv2_i(
    <2 x i32> %x, <2 x i32> %y) #0
{
    %x0 = extractelement <2 x i32> %x, i32 0
    %x1 = extractelement <2 x i32> %x, i32 1

    %y0 = extractelement <2 x i32> %y, i32 0
    %y1 = extractelement <2 x i32> %y, i32 1

    %1 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x0, i32 %y0)
    %2 = extractvalue { i32, i1 } %1, 0
    %3 = extractvalue { i32, i1 } %1, 1
    %4 = zext i1 %3 to i32

    %5 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x1, i32 %y1)
    %6 = extractvalue { i32, i1 } %5, 0
    %7 = extractvalue { i32, i1 } %5, 1
    %8 = zext i1 %7 to i32

    %9 = insertelement <2 x i32> undef, i32 %2, i32 0
    %10 = insertelement <2 x i32> %9, i32 %6, i32 1

    %11 = insertelement <2 x i32> undef, i32 %4, i32 0
    %12 = insertelement <2 x i32> %11, i32 %8, i32 1

    %13 = insertvalue { <2 x i32>, <2 x i32> } undef, <2 x i32> %10, 0
    %14 = insertvalue { <2 x i32>, <2 x i32> } %13, <2 x i32> %12, 1

    ret {<2 x i32>, <2 x i32>} %14
}

; GLSL: uvec3 usubBorrow(uvec3, uvec3, out uvec3)
define spir_func {<3 x i32>, <3 x i32>} @_Z10ISubBorrowDv3_iDv3_i(
    <3 x i32> %x, <3 x i32> %y) #0
{
    %x0 = extractelement <3 x i32> %x, i32 0
    %x1 = extractelement <3 x i32> %x, i32 1
    %x2 = extractelement <3 x i32> %x, i32 2

    %y0 = extractelement <3 x i32> %y, i32 0
    %y1 = extractelement <3 x i32> %y, i32 1
    %y2 = extractelement <3 x i32> %y, i32 2

    %1 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x0, i32 %y0)
    %2 = extractvalue { i32, i1 } %1, 0
    %3 = extractvalue { i32, i1 } %1, 1
    %4 = zext i1 %3 to i32

    %5 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x1, i32 %y1)
    %6 = extractvalue { i32, i1 } %5, 0
    %7 = extractvalue { i32, i1 } %5, 1
    %8 = zext i1 %7 to i32

    %9 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x2, i32 %y2)
    %10 = extractvalue { i32, i1 } %9, 0
    %11 = extractvalue { i32, i1 } %9, 1
    %12 = zext i1 %11 to i32

    %13 = insertelement <3 x i32> undef, i32 %2, i32 0
    %14 = insertelement <3 x i32> %13, i32 %6, i32 1
    %15 = insertelement <3 x i32> %14, i32 %10, i32 2

    %16 = insertelement <3 x i32> undef, i32 %4, i32 0
    %17 = insertelement <3 x i32> %16, i32 %8, i32 1
    %18 = insertelement <3 x i32> %17, i32 %12, i32 2

    %19 = insertvalue { <3 x i32>, <3 x i32> } undef, <3 x i32> %15, 0
    %20 = insertvalue { <3 x i32>, <3 x i32> } %19, <3 x i32> %18, 1

    ret {<3 x i32>, <3 x i32>} %20
}

; GLSL: uvec4 usubBorrow(uvec4, uvec4, out uvec4)
define spir_func {<4 x i32>, <4 x i32>} @_Z10ISubBorrowDv4_iDv4_i(
    <4 x i32> %x, <4 x i32> %y) #0
{
    %x0 = extractelement <4 x i32> %x, i32 0
    %x1 = extractelement <4 x i32> %x, i32 1
    %x2 = extractelement <4 x i32> %x, i32 2
    %x3 = extractelement <4 x i32> %x, i32 3

    %y0 = extractelement <4 x i32> %y, i32 0
    %y1 = extractelement <4 x i32> %y, i32 1
    %y2 = extractelement <4 x i32> %y, i32 2
    %y3 = extractelement <4 x i32> %y, i32 3

    %1 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x0, i32 %y0)
    %2 = extractvalue { i32, i1 } %1, 0
    %3 = extractvalue { i32, i1 } %1, 1
    %4 = zext i1 %3 to i32

    %5 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x1, i32 %y1)
    %6 = extractvalue { i32, i1 } %5, 0
    %7 = extractvalue { i32, i1 } %5, 1
    %8 = zext i1 %7 to i32

    %9 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x2, i32 %y2)
    %10 = extractvalue { i32, i1 } %9, 0
    %11 = extractvalue { i32, i1 } %9, 1
    %12 = zext i1 %11 to i32

    %13 = call { i32, i1 } @llvm.usub.with.overflow.i32(i32 %x3, i32 %y3)
    %14 = extractvalue { i32, i1 } %13, 0
    %15 = extractvalue { i32, i1 } %13, 1
    %16 = zext i1 %15 to i32

    %17 = insertelement <4 x i32> undef, i32 %2, i32 0
    %18 = insertelement <4 x i32> %17, i32 %6, i32 1
    %19 = insertelement <4 x i32> %18, i32 %10, i32 2
    %20 = insertelement <4 x i32> %19, i32 %14, i32 3

    %21 = insertelement <4 x i32> undef, i32 %4, i32 0
    %22 = insertelement <4 x i32> %21, i32 %8, i32 1
    %23 = insertelement <4 x i32> %22, i32 %12, i32 2
    %24 = insertelement <4 x i32> %23, i32 %16, i32 3

    %25 = insertvalue { <4 x i32>, <4 x i32> } undef, <4 x i32> %20, 0
    %26 = insertvalue { <4 x i32>, <4 x i32> } %25, <4 x i32> %24, 1

    ret {<4 x i32>, <4 x i32>} %26
}

; GLSL: void umulExtended(uint, uint, out uint, out uint)
define spir_func {i32, i32} @_Z12UMulExtendedii(
    i32 %x, i32 %y) #0
{
    %1 = zext i32 %x to i64
    %2 = zext i32 %y to i64
    %3 = mul i64 %1, %2
    %4 = trunc i64 %3 to i32
    %5 = lshr i64 %3, 32
    %6 = trunc i64 %5 to i32

    %7 = insertvalue { i32, i32 } undef, i32 %4, 0
    %8 = insertvalue { i32, i32 } %7, i32 %6, 1

    ret {i32, i32} %8
}

; GLSL: void umulExtended(uvec2, uvec2, out uvec2, out uvec2)
define spir_func {<2 x i32>, <2 x i32>} @_Z12UMulExtendedDv2_iDv2_i(
    <2 x i32> %x, <2 x i32> %y) #0
{
    %x0 = extractelement <2 x i32> %x, i32 0
    %x1 = extractelement <2 x i32> %x, i32 1

    %y0 = extractelement <2 x i32> %y, i32 0
    %y1 = extractelement <2 x i32> %y, i32 1

    %1 = zext i32 %x0 to i64
    %2 = zext i32 %y0 to i64
    %3 = mul i64 %1, %2
    %4 = trunc i64 %3 to i32
    %5 = lshr i64 %3, 32
    %6 = trunc i64 %5 to i32

    %7 = zext i32 %x1 to i64
    %8 = zext i32 %y1 to i64
    %9 = mul i64 %7, %8
    %10 = trunc i64 %9 to i32
    %11 = lshr i64 %9, 32
    %12 = trunc i64 %11 to i32

    %13 = insertelement <2 x i32> undef, i32 %4, i32 0
    %14 = insertelement <2 x i32> %13, i32 %10, i32 1

    %15 = insertelement <2 x i32> undef, i32 %6, i32 0
    %16 = insertelement <2 x i32> %15, i32 %12, i32 1

    %17 = insertvalue { <2 x i32>, <2 x i32> } undef, <2 x i32> %14, 0
    %18 = insertvalue { <2 x i32>, <2 x i32> } %17, <2 x i32> %16, 1

    ret {<2 x i32>, <2 x i32>} %18
}

; GLSL: void umulExtended(uvec3, uvec3, out uvec3, out uvec3)
define spir_func {<3 x i32>, <3 x i32>} @_Z12UMulExtendedDv3_iDv3_i(
    <3 x i32> %x, <3 x i32> %y) #0
{
    %x0 = extractelement <3 x i32> %x, i32 0
    %x1 = extractelement <3 x i32> %x, i32 1
    %x2 = extractelement <3 x i32> %x, i32 2

    %y0 = extractelement <3 x i32> %y, i32 0
    %y1 = extractelement <3 x i32> %y, i32 1
    %y2 = extractelement <3 x i32> %y, i32 2

    %1 = zext i32 %x0 to i64
    %2 = zext i32 %y0 to i64
    %3 = mul i64 %1, %2
    %4 = trunc i64 %3 to i32
    %5 = lshr i64 %3, 32
    %6 = trunc i64 %5 to i32

    %7 = zext i32 %x1 to i64
    %8 = zext i32 %y1 to i64
    %9 = mul i64 %7, %8
    %10 = trunc i64 %9 to i32
    %11 = lshr i64 %9, 32
    %12 = trunc i64 %11 to i32

    %13 = zext i32 %x2 to i64
    %14 = zext i32 %y2 to i64
    %15 = mul i64 %13, %14
    %16 = trunc i64 %15 to i32
    %17 = lshr i64 %15, 32
    %18 = trunc i64 %17 to i32

    %19 = insertelement <3 x i32> undef, i32 %4, i32 0
    %20 = insertelement <3 x i32> %19, i32 %10, i32 1
    %21 = insertelement <3 x i32> %20, i32 %16, i32 2

    %22 = insertelement <3 x i32> undef, i32 %6, i32 0
    %23 = insertelement <3 x i32> %22, i32 %12, i32 1
    %24 = insertelement <3 x i32> %23, i32 %18, i32 2

    %25 = insertvalue { <3 x i32>, <3 x i32> } undef, <3 x i32> %21, 0
    %26 = insertvalue { <3 x i32>, <3 x i32> } %25, <3 x i32> %24, 1

    ret {<3 x i32>, <3 x i32>} %26
}

; GLSL: void umulExtended(uvec4, uvec4, out uvec4, out uvec4)
define spir_func {<4 x i32>, <4 x i32>} @_Z12UMulExtendedDv4_iDv4_i(
    <4 x i32> %x, <4 x i32> %y) #0
{
    %x0 = extractelement <4 x i32> %x, i32 0
    %x1 = extractelement <4 x i32> %x, i32 1
    %x2 = extractelement <4 x i32> %x, i32 2
    %x3 = extractelement <4 x i32> %x, i32 3

    %y0 = extractelement <4 x i32> %y, i32 0
    %y1 = extractelement <4 x i32> %y, i32 1
    %y2 = extractelement <4 x i32> %y, i32 2
    %y3 = extractelement <4 x i32> %y, i32 3

    %1 = zext i32 %x0 to i64
    %2 = zext i32 %y0 to i64
    %3 = mul i64 %1, %2
    %4 = trunc i64 %3 to i32
    %5 = lshr i64 %3, 32
    %6 = trunc i64 %5 to i32

    %7 = zext i32 %x1 to i64
    %8 = zext i32 %y1 to i64
    %9 = mul i64 %7, %8
    %10 = trunc i64 %9 to i32
    %11 = lshr i64 %9, 32
    %12 = trunc i64 %11 to i32

    %13 = zext i32 %x2 to i64
    %14 = zext i32 %y2 to i64
    %15 = mul i64 %13, %14
    %16 = trunc i64 %15 to i32
    %17 = lshr i64 %15, 32
    %18 = trunc i64 %17 to i32

    %19 = zext i32 %x3 to i64
    %20 = zext i32 %y3 to i64
    %21 = mul i64 %19, %20
    %22 = trunc i64 %21 to i32
    %23 = lshr i64 %21, 32
    %24 = trunc i64 %23 to i32

    %25 = insertelement <4 x i32> undef, i32 %4, i32 0
    %26 = insertelement <4 x i32> %25, i32 %10, i32 1
    %27 = insertelement <4 x i32> %26, i32 %16, i32 2
    %28 = insertelement <4 x i32> %27, i32 %22, i32 3

    %29 = insertelement <4 x i32> undef, i32 %6, i32 0
    %30 = insertelement <4 x i32> %29, i32 %12, i32 1
    %31 = insertelement <4 x i32> %30, i32 %18, i32 2
    %32 = insertelement <4 x i32> %31, i32 %24, i32 3

    %33 = insertvalue { <4 x i32>, <4 x i32> } undef, <4 x i32> %28, 0
    %34 = insertvalue { <4 x i32>, <4 x i32> } %33, <4 x i32> %32, 1

    ret {<4 x i32>, <4 x i32>} %34
}

; GLSL: void imulExtended(int, int, out int, out int)
define spir_func {i32, i32} @_Z12SMulExtendedii(
    i32 %x, i32 %y) #0
{
    %1 = sext i32 %x to i64
    %2 = sext i32 %y to i64
    %3 = mul i64 %1, %2
    %4 = trunc i64 %3 to i32
    %5 = lshr i64 %3, 32
    %6 = trunc i64 %5 to i32

    %7 = insertvalue { i32, i32 } undef, i32 %4, 0
    %8 = insertvalue { i32, i32 } %7, i32 %6, 1

    ret {i32, i32} %8
}

; GLSL: void imulExtended(ivec2, ivec2, out ivec2, out ivec2)
define spir_func {<2 x i32>, <2 x i32>} @_Z12SMulExtendedDv2_iDv2_i(
    <2 x i32> %x, <2 x i32> %y) #0
{
    %x0 = extractelement <2 x i32> %x, i32 0
    %x1 = extractelement <2 x i32> %x, i32 1

    %y0 = extractelement <2 x i32> %y, i32 0
    %y1 = extractelement <2 x i32> %y, i32 1

    %1 = sext i32 %x0 to i64
    %2 = sext i32 %y0 to i64
    %3 = mul i64 %1, %2
    %4 = trunc i64 %3 to i32
    %5 = lshr i64 %3, 32
    %6 = trunc i64 %5 to i32

    %7 = sext i32 %x1 to i64
    %8 = sext i32 %y1 to i64
    %9 = mul i64 %7, %8
    %10 = trunc i64 %9 to i32
    %11 = lshr i64 %9, 32
    %12 = trunc i64 %11 to i32

    %13 = insertelement <2 x i32> undef, i32 %4, i32 0
    %14 = insertelement <2 x i32> %13, i32 %10, i32 1

    %15 = insertelement <2 x i32> undef, i32 %6, i32 0
    %16 = insertelement <2 x i32> %15, i32 %12, i32 1

    %17 = insertvalue { <2 x i32>, <2 x i32> } undef, <2 x i32> %14, 0
    %18 = insertvalue { <2 x i32>, <2 x i32> } %17, <2 x i32> %16, 1

    ret {<2 x i32>, <2 x i32>} %18
}

; GLSL: void imulExtended(ivec3, ivec3, out ivec3, out ivec3)
define spir_func {<3 x i32>, <3 x i32>} @_Z12SMulExtendedDv3_iDv3_i(
    <3 x i32> %x, <3 x i32> %y) #0
{
    %x0 = extractelement <3 x i32> %x, i32 0
    %x1 = extractelement <3 x i32> %x, i32 1
    %x2 = extractelement <3 x i32> %x, i32 2

    %y0 = extractelement <3 x i32> %y, i32 0
    %y1 = extractelement <3 x i32> %y, i32 1
    %y2 = extractelement <3 x i32> %y, i32 2

    %1 = sext i32 %x0 to i64
    %2 = sext i32 %y0 to i64
    %3 = mul i64 %1, %2
    %4 = trunc i64 %3 to i32
    %5 = lshr i64 %3, 32
    %6 = trunc i64 %5 to i32

    %7 = sext i32 %x1 to i64
    %8 = sext i32 %y1 to i64
    %9 = mul i64 %7, %8
    %10 = trunc i64 %9 to i32
    %11 = lshr i64 %9, 32
    %12 = trunc i64 %11 to i32

    %13 = sext i32 %x2 to i64
    %14 = sext i32 %y2 to i64
    %15 = mul i64 %13, %14
    %16 = trunc i64 %15 to i32
    %17 = lshr i64 %15, 32
    %18 = trunc i64 %17 to i32

    %19 = insertelement <3 x i32> undef, i32 %4, i32 0
    %20 = insertelement <3 x i32> %19, i32 %10, i32 1
    %21 = insertelement <3 x i32> %20, i32 %16, i32 2

    %22 = insertelement <3 x i32> undef, i32 %6, i32 0
    %23 = insertelement <3 x i32> %22, i32 %12, i32 1
    %24 = insertelement <3 x i32> %23, i32 %18, i32 2

    %25 = insertvalue { <3 x i32>, <3 x i32> } undef, <3 x i32> %21, 0
    %26 = insertvalue { <3 x i32>, <3 x i32> } %25, <3 x i32> %24, 1

    ret {<3 x i32>, <3 x i32>} %26
}

; GLSL: void imulExtended(ivec4, ivec4, out ivec4, out ivec4)
define spir_func {<4 x i32>, <4 x i32>} @_Z12SMulExtendedDv4_iDv4_i(
    <4 x i32> %x, <4 x i32> %y) #0
{
    %x0 = extractelement <4 x i32> %x, i32 0
    %x1 = extractelement <4 x i32> %x, i32 1
    %x2 = extractelement <4 x i32> %x, i32 2
    %x3 = extractelement <4 x i32> %x, i32 3

    %y0 = extractelement <4 x i32> %y, i32 0
    %y1 = extractelement <4 x i32> %y, i32 1
    %y2 = extractelement <4 x i32> %y, i32 2
    %y3 = extractelement <4 x i32> %y, i32 3

    %1 = sext i32 %x0 to i64
    %2 = sext i32 %y0 to i64
    %3 = mul i64 %1, %2
    %4 = trunc i64 %3 to i32
    %5 = lshr i64 %3, 32
    %6 = trunc i64 %5 to i32

    %7 = sext i32 %x1 to i64
    %8 = sext i32 %y1 to i64
    %9 = mul i64 %7, %8
    %10 = trunc i64 %9 to i32
    %11 = lshr i64 %9, 32
    %12 = trunc i64 %11 to i32

    %13 = sext i32 %x2 to i64
    %14 = sext i32 %y2 to i64
    %15 = mul i64 %13, %14
    %16 = trunc i64 %15 to i32
    %17 = lshr i64 %15, 32
    %18 = trunc i64 %17 to i32

    %19 = sext i32 %x3 to i64
    %20 = sext i32 %y3 to i64
    %21 = mul i64 %19, %20
    %22 = trunc i64 %21 to i32
    %23 = lshr i64 %21, 32
    %24 = trunc i64 %23 to i32

    %25 = insertelement <4 x i32> undef, i32 %4, i32 0
    %26 = insertelement <4 x i32> %25, i32 %10, i32 1
    %27 = insertelement <4 x i32> %26, i32 %16, i32 2
    %28 = insertelement <4 x i32> %27, i32 %22, i32 3

    %29 = insertelement <4 x i32> undef, i32 %6, i32 0
    %30 = insertelement <4 x i32> %29, i32 %12, i32 1
    %31 = insertelement <4 x i32> %30, i32 %18, i32 2
    %32 = insertelement <4 x i32> %31, i32 %24, i32 3

    %33 = insertvalue { <4 x i32>, <4 x i32> } undef, <4 x i32> %28, 0
    %34 = insertvalue { <4 x i32>, <4 x i32> } %33, <4 x i32> %32, 1

    ret {<4 x i32>, <4 x i32>} %34
}

declare float @llvm.amdgcn.fmul.legacy(float, float) #1
declare i32 @llvm.amdgcn.sbfe.i32(i32, i32, i32) #1
declare i32 @llvm.amdgcn.ubfe.i32(i32, i32, i32) #1
declare float @llvm.trunc.f32(float ) #0
declare { i32, i1 } @llvm.uadd.with.overflow.i32(i32, i32) #0
declare { i32, i1 } @llvm.usub.with.overflow.i32(i32, i32) #0
declare float @llvm.exp2.f32(float) #0
declare float @llvm.log2.f32(float) #0
declare float @llvm.sin.f32(float) #0
declare float @llvm.cos.f32(float) #0
declare float @llvm.sqrt.f32(float) #0
declare float @llvm.floor.f32(float) #0
declare float @llvm.pow.f32(float, float) #0
declare float @llvm.minnum.f32(float, float) #0
declare float @llvm.maxnum.f32(float, float) #0
declare float @llvm.fabs.f32(float) #0
declare i32 @llvm.cttz.i32(i32, i1) #0
declare i32 @llvm.ctlz.i32(i32, i1) #0
declare i32 @llvm.amdgcn.sffbh.i32(i32) #1
declare i1 @llvm.amdgcn.class.f32(float, i32) #1
declare float @llvm.amdgcn.frexp.mant.f32(float) #1
declare i32 @llvm.amdgcn.frexp.exp.i32.f32(float) #1
declare i32 @llvm.amdgcn.cvt.pk.u8.f32(float, i32, i32) #1
declare float @llvm.amdgcn.fdiv.fast(float, float) #1
declare float @llvm.amdgcn.fract.f32(float) #1
declare float @llvm.amdgcn.fmed3.f32(float, float, float) #1
declare float @llvm.rint.f32(float) #0
declare float @llvm.fmuladd.f32(float, float, float) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
