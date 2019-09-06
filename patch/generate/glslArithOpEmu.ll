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

; GLSL: int abs(int)
define i32 @llpc.sabs.i32(i32 %x) #0
{
    %nx = sub i32 0, %x
    %con = icmp sgt i32 %x, %nx
    %val = select i1 %con, i32 %x, i32 %nx
    ret i32 %val
}

; GLSL: float sign(float)
define float @llpc.fsign.f32(float %x) #0
{
    %con1 = fcmp ogt float %x, 0.0
    %ret1 = select i1 %con1, float 1.0, float %x
    %con2 = fcmp oge float %ret1, 0.0
    %ret2 = select i1 %con2, float %ret1, float -1.0
    ret float %ret2
}

; GLSL: int sign(int)
define i32 @llpc.ssign.i32(i32 %x) #0
{
    %con1 = icmp sgt i32 %x, 0
    %ret1 = select i1 %con1, i32 1, i32 %x
    %con2 = icmp sge i32 %ret1, 0
    %ret2 = select i1 %con2, i32 %ret1, i32 -1
    ret i32 %ret2
}

; GLSL: float round(float)
define float @llpc.round.f32(float %x)
{
    %1 = call float @llvm.rint.f32(float %x)
    ret float %1
}

; GLSL: float fract(float)
define float @llpc.fract.f32(float %x) #0
{
    %1 = call float @llvm.amdgcn.fract.f32(float %x)
    ret float %1
}

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

; GLSL: float modf(float, out float)
define spir_func float @_Z4modffPf(
    float %x, float addrspace(5)* %i) #0
{
    %1 = call float @llvm.trunc.f32(float %x)
    %2 = fsub float %x, %1

    store float %1, float addrspace(5)* %i
    ret float %2
}

; GLSL: vec2 modf(vec2, out vec2)
define spir_func <2 x float> @_Z4modfDv2_fPDv2_f(
    <2 x float> %x, <2 x float> addrspace(5)* %i) #0
{
    %x0 = extractelement <2 x float> %x, i32 0
    %x1 = extractelement <2 x float> %x, i32 1

    %1 = call float @llvm.trunc.f32(float %x0)
    %2 = fsub float %x0, %1

    %3 = call float @llvm.trunc.f32(float %x1)
    %4 = fsub float %x1, %3

    %5 = insertelement <2 x float> undef, float %1, i32 0
    %6 = insertelement <2 x float> %5, float %3, i32 1

    %7 = insertelement <2 x float> undef, float %2, i32 0
    %8 = insertelement <2 x float> %7, float %4, i32 1

    store <2 x float> %6, <2 x float> addrspace(5)* %i
    ret <2 x float> %8
}

; GLSL: vec3 modf(vec3, out vec3)
define spir_func <3 x float> @_Z4modfDv3_fPDv3_f(
    <3 x float> %x, <3 x float> addrspace(5)* %i) #0
{
    %x0 = extractelement <3 x float> %x, i32 0
    %x1 = extractelement <3 x float> %x, i32 1
    %x2 = extractelement <3 x float> %x, i32 2

    %1 = call float @llvm.trunc.f32(float %x0)
    %2 = fsub float %x0, %1

    %3 = call float @llvm.trunc.f32(float %x1)
    %4 = fsub float %x1, %3

    %5 = call float @llvm.trunc.f32(float %x2)
    %6 = fsub float %x2, %5

    %7 = insertelement <3 x float> undef, float %1, i32 0
    %8 = insertelement <3 x float> %7, float %3, i32 1
    %9 = insertelement <3 x float> %8, float %5, i32 2

    %10 = insertelement <3 x float> undef, float %2, i32 0
    %11 = insertelement <3 x float> %10, float %4, i32 1
    %12 = insertelement <3 x float> %11, float %6, i32 2

    store <3 x float> %9, <3 x float> addrspace(5)* %i
    ret <3 x float> %12
}

; GLSL: vec4 modf(vec4, out vec4)
define spir_func <4 x float> @_Z4modfDv4_fPDv4_f(
    <4 x float> %x, <4 x float> addrspace(5)* %i) #0
{
    %x0 = extractelement <4 x float> %x, i32 0
    %x1 = extractelement <4 x float> %x, i32 1
    %x2 = extractelement <4 x float> %x, i32 2
    %x3 = extractelement <4 x float> %x, i32 3

    %1 = call float @llvm.trunc.f32(float %x0)
    %2 = fsub float %x0, %1

    %3 = call float @llvm.trunc.f32(float %x1)
    %4 = fsub float %x1, %3

    %5 = call float @llvm.trunc.f32(float %x2)
    %6 = fsub float %x2, %5

    %7 = call float @llvm.trunc.f32(float %x3)
    %8 = fsub float %x3, %7

    %9 = insertelement <4 x float> undef, float %1, i32 0
    %10 = insertelement <4 x float> %9, float %3, i32 1
    %11 = insertelement <4 x float> %10, float %5, i32 2
    %12 = insertelement <4 x float> %11, float %7, i32 3

    %13 = insertelement <4 x float> undef, float %2, i32 0
    %14 = insertelement <4 x float> %13, float %4, i32 1
    %15 = insertelement <4 x float> %14, float %6, i32 2
    %16 = insertelement <4 x float> %15, float %8, i32 3

    store <4 x float> %12, <4 x float> addrspace(5)* %i
    ret <4 x float> %16
}

; GLSL: float modf(float, out float)
define spir_func { float, float } @_Z10modfStructf(float %x) #0
{
    %1 = call float @llvm.trunc.f32(float %x)
    %2 = fsub float %x, %1

    %3 = insertvalue { float, float } undef, float %2, 0
    %4 = insertvalue { float, float } %3, float %1, 1

    ret { float, float } %4
}

; GLSL: vec2 modf(vec2, out vec2)
define spir_func { <2 x float>, <2 x float> } @_Z10modfStructDv2_f(<2 x float> %x) #0
{
    %x0 = extractelement <2 x float> %x, i32 0
    %x1 = extractelement <2 x float> %x, i32 1

    %1 = call float @llvm.trunc.f32(float %x0)
    %2 = fsub float %x0, %1

    %3 = call float @llvm.trunc.f32(float %x1)
    %4 = fsub float %x1, %3

    %5 = insertelement <2 x float> undef, float %1, i32 0
    %6 = insertelement <2 x float> %5, float %3, i32 1

    %7 = insertelement <2 x float> undef, float %2, i32 0
    %8 = insertelement <2 x float> %7, float %4, i32 1

    %9 = insertvalue { <2 x float>, <2 x float> } undef, <2 x float> %8, 0
    %10 = insertvalue { <2 x float>, <2 x float> } %9, <2 x float> %6, 1

    ret { <2 x float>, <2 x float> } %10
}

; GLSL: vec3 modf(vec3, out vec3)
define spir_func { <3 x float>, <3 x float> } @_Z10modfStructDv3_f(<3 x float> %x) #0
{
    %x0 = extractelement <3 x float> %x, i32 0
    %x1 = extractelement <3 x float> %x, i32 1
    %x2 = extractelement <3 x float> %x, i32 2

    %1 = call float @llvm.trunc.f32(float %x0)
    %2 = fsub float %x0, %1

    %3 = call float @llvm.trunc.f32(float %x1)
    %4 = fsub float %x1, %3

    %5 = call float @llvm.trunc.f32(float %x2)
    %6 = fsub float %x2, %5

    %7 = insertelement <3 x float> undef, float %1, i32 0
    %8 = insertelement <3 x float> %7, float %3, i32 1
    %9 = insertelement <3 x float> %8, float %5, i32 2

    %10 = insertelement <3 x float> undef, float %2, i32 0
    %11 = insertelement <3 x float> %10, float %4, i32 1
    %12 = insertelement <3 x float> %11, float %6, i32 2

    %13 = insertvalue { <3 x float>, <3 x float> } undef, <3 x float> %12, 0
    %14 = insertvalue { <3 x float>, <3 x float> } %13, <3 x float> %9, 1

    ret { <3 x float>, <3 x float> } %14
}

; GLSL: vec4 modf(vec4, out vec4)
define spir_func { <4 x float>, <4 x float> } @_Z10modfStructDv4_f(<4 x float> %x) #0
{
    %x0 = extractelement <4 x float> %x, i32 0
    %x1 = extractelement <4 x float> %x, i32 1
    %x2 = extractelement <4 x float> %x, i32 2
    %x3 = extractelement <4 x float> %x, i32 3

    %1 = call float @llvm.trunc.f32(float %x0)
    %2 = fsub float %x0, %1

    %3 = call float @llvm.trunc.f32(float %x1)
    %4 = fsub float %x1, %3

    %5 = call float @llvm.trunc.f32(float %x2)
    %6 = fsub float %x2, %5

    %7 = call float @llvm.trunc.f32(float %x3)
    %8 = fsub float %x3, %7

    %9 = insertelement <4 x float> undef, float %1, i32 0
    %10 = insertelement <4 x float> %9, float %3, i32 1
    %11 = insertelement <4 x float> %10, float %5, i32 2
    %12 = insertelement <4 x float> %11, float %7, i32 3

    %13 = insertelement <4 x float> undef, float %2, i32 0
    %14 = insertelement <4 x float> %13, float %4, i32 1
    %15 = insertelement <4 x float> %14, float %6, i32 2
    %16 = insertelement <4 x float> %15, float %8, i32 3

    %17 = insertvalue { <4 x float>, <4 x float> } undef, <4 x float> %16, 0
    %18 = insertvalue { <4 x float>, <4 x float> } %17, <4 x float> %12, 1

    ret { <4 x float>, <4 x float> } %18
}

; GLSL: float nmin(float, float)
define float @llpc.nmin.f32(float %x, float %y) #0
{
    %1 = call float @llvm.minnum.f32(float %x, float %y)
    ret float %1
}

; GLSL: int min(int, int)
define i32 @llpc.sminnum.i32(i32 %x, i32 %y) #0
{
    %1 = icmp slt i32 %y, %x
    %2 = select i1 %1, i32 %y, i32 %x
    ret i32 %2
}

; GLSL: uint min(uint, uint)
define i32 @llpc.uminnum.i32(i32 %x, i32 %y) #0
{
    %1 = icmp ult i32 %y, %x
    %2 = select i1 %1, i32 %y, i32 %x
    ret i32 %2
}

; GLSL: float nmax(float, float)
define float @llpc.nmax.f32(float %x, float %y) #0
{
    %1 = call float @llvm.maxnum.f32(float %x, float %y)
    ret float %1
}

; GLSL: int max(int, int)
define i32 @llpc.smaxnum.i32(i32 %x, i32 %y) #0
{
    %1 = icmp slt i32 %x, %y
    %2 = select i1 %1, i32 %y, i32 %x
    ret i32 %2
}

; GLSL: uint max(uint, uint)
define i32 @llpc.umaxnum.i32(i32 %x, i32 %y) #0
{
    %1 = icmp ult i32 %x, %y
    %2 = select i1 %1, i32 %y, i32 %x
    ret i32 %2
}

; GLSL: int clamp(int, int, int)
define i32 @llpc.sclamp.i32(i32 %x, i32 %minVal, i32 %maxVal) #0
{
    %1 = call i32 @llpc.smaxnum.i32(i32 %x, i32 %minVal)
    %2 = call i32 @llpc.sminnum.i32(i32 %1, i32 %maxVal)
    ret i32 %2
}

; GLSL: uint clamp(uint, uint, uint)
define i32 @llpc.uclamp.i32(i32 %x, i32 %minVal, i32 %maxVal) #0
{
    %1 = call i32 @llpc.umaxnum.i32(i32 %x, i32 %minVal)
    %2 = call i32 @llpc.uminnum.i32(i32 %1, i32 %maxVal)
    ret i32 %2
}

; GLSL: float clamp(float, float, float)
define float @llpc.fclamp.f32(float %x, float %minVal, float %maxVal) #0
{
    %1 = call float @llvm.amdgcn.fmed3.f32(float %x, float %minVal, float %maxVal)
    ret float %1
}

; GLSL: float nclamp(float, float, float)
define float @llpc.nclamp.f32(float %x, float %minVal, float %maxVal) #0
{
    %1 = call float @llvm.amdgcn.fmed3.f32(float %x, float %minVal, float %maxVal)
    ret float %1
}

; GLSL: float mix(float, float, float)
define float @llpc.fmix.f32(float %x, float %y, float %a) #0
{
    %1 = fsub float %y, %x
    %2 = tail call float @llvm.fmuladd.f32(float %1, float %a, float %x)

    ret float %2
}

; GLSL: float step(float, float)
define float @llpc.step.f32(float %edge, float %x) #0
{
    %1 = fcmp olt float %x, %edge
    %2 = select i1 %1, float 0.0, float 1.0
    ret float %2
}

; GLSL: float smoothstep(float, float, float)
define float @llpc.smoothStep.f32(float %edge0, float %edge1, float %x) #0
{
    %1 = fsub float %x, %edge0
    %2 = fsub float %edge1, %edge0
    %3 = fdiv float 1.0, %2
    %4 = fmul float %1, %3
    %5 = call float @llpc.fclamp.f32(float %4, float 0.0, float 1.0)
    %6 = fmul float %5, %5
    %7 = fmul float -2.0, %5
    %8 = fadd float 3.0, %7
    %9 = fmul float %6, %8
    ret float %9
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

; GLSL: float frexp(float, out int)
define spir_func {float, i32} @_Z11frexpStructf(
    float %x) #0
{
    %1 = call float @llvm.amdgcn.frexp.mant.f32(float %x)
    %2 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x)

    %3 = insertvalue { float, i32 } undef, float %1, 0
    %4 = insertvalue { float, i32 } %3, i32 %2, 1

    ret {float, i32} %4
}

; GLSL: vec2 frexp(vec2, out ivec2)
define spir_func {<2 x float>, <2 x i32>} @_Z11frexpStructDv2_f(
    <2 x float> %x) #0
{
    %x0 = extractelement <2 x float> %x, i32 0
    %x1 = extractelement <2 x float> %x, i32 1

    %1 = call float @llvm.amdgcn.frexp.mant.f32(float %x0)
    %2 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x0)

    %3 = call float @llvm.amdgcn.frexp.mant.f32(float %x1)
    %4 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x1)

    %5 = insertelement <2 x float> undef, float %1, i32 0
    %6 = insertelement <2 x float> %5, float %3, i32 1

    %7 = insertelement <2 x i32> undef, i32 %2, i32 0
    %8 = insertelement <2 x i32> %7, i32 %4, i32 1

    %9  = insertvalue { <2 x float>, <2 x i32> } undef, <2 x float> %6, 0
    %10 = insertvalue { <2 x float>, <2 x i32> } %9, <2 x i32> %8, 1

    ret {<2 x float>, <2 x i32>} %10
}

; GLSL: vec3 frexp(vec3, out ivec3)
define spir_func {<3 x float>, <3 x i32>} @_Z11frexpStructDv3_f(
    <3 x float> %x) #0
{
    %x0 = extractelement <3 x float> %x, i32 0
    %x1 = extractelement <3 x float> %x, i32 1
    %x2 = extractelement <3 x float> %x, i32 2

    %1 = call float @llvm.amdgcn.frexp.mant.f32(float %x0)
    %2 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x0)

    %3 = call float @llvm.amdgcn.frexp.mant.f32(float %x1)
    %4 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x1)

    %5 = call float @llvm.amdgcn.frexp.mant.f32(float %x2)
    %6 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x2)

    %7 = insertelement <3 x float> undef, float %1, i32 0
    %8 = insertelement <3 x float> %7, float %3, i32 1
    %9 = insertelement <3 x float> %8, float %5, i32 2

    %10 = insertelement <3 x i32> undef, i32 %2, i32 0
    %11 = insertelement <3 x i32> %10, i32 %4, i32 1
    %12 = insertelement <3 x i32> %11, i32 %6, i32 2

    %13 = insertvalue { <3 x float>, <3 x i32> } undef, <3 x float> %9, 0
    %14 = insertvalue { <3 x float>, <3 x i32> } %13, <3 x i32> %12, 1

    ret {<3 x float>, <3 x i32>} %14
}

; GLSL: vec4 frexp(vec4, out ivec4)
define spir_func {<4 x float>, <4 x i32>} @_Z11frexpStructDv4_f(
    <4 x float> %x) #0
{
    %x0 = extractelement <4 x float> %x, i32 0
    %x1 = extractelement <4 x float> %x, i32 1
    %x2 = extractelement <4 x float> %x, i32 2
    %x3 = extractelement <4 x float> %x, i32 3

    %1 = call float @llvm.amdgcn.frexp.mant.f32(float %x0)
    %2 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x0)

    %3 = call float @llvm.amdgcn.frexp.mant.f32(float %x1)
    %4 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x1)

    %5 = call float @llvm.amdgcn.frexp.mant.f32(float %x2)
    %6 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x2)

    %7 = call float @llvm.amdgcn.frexp.mant.f32(float %x3)
    %8 = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float %x3)

    %9 = insertelement <4 x float> undef, float %1, i32 0
    %10 = insertelement <4 x float> %9, float %3, i32 1
    %11 = insertelement <4 x float> %10, float %5, i32 2
    %12 = insertelement <4 x float> %11, float %7, i32 3

    %13 = insertelement <4 x i32> undef, i32 %2, i32 0
    %14 = insertelement <4 x i32> %13, i32 %4, i32 1
    %15 = insertelement <4 x i32> %14, i32 %6, i32 2
    %16 = insertelement <4 x i32> %15, i32 %8, i32 3

    %17 = insertvalue { <4 x float>, <4 x i32> } undef, <4 x float> %12, 0
    %18 = insertvalue { <4 x float>, <4 x i32> } %17, <4 x i32> %16, 1

    ret {<4 x float>, <4 x i32>} %18
}

; GLSL: float fma(float, float, float)
define float @llpc.fma.f32(float %a, float %b, float %c) #0
{
    %1 = tail call float @llvm.fmuladd.f32(float %a, float %b, float %c)
    ret float %1
}

; =====================================================================================================================
; >>>  Floating-point Pack and Unpack Functions
; =====================================================================================================================

; GLSL: uint packUnorm2x16(vec2)
define i32 @_Z13packUnorm2x16Dv2_f(<2 x float> %v) #0
{
    %v0 = extractelement <2 x float> %v, i32 0
    %v1 = extractelement <2 x float> %v, i32 1

    %vc0 = call float @llpc.fclamp.f32(float %v0, float 0.0, float 1.0)
    %vc1 = call float @llpc.fclamp.f32(float %v1, float 0.0, float 1.0)

    %vc0elv = fmul float %vc0, 65535.0
    %vc1elv = fmul float %vc1, 65535.0

    %vc0itg = fptoui float %vc0elv to i32
    %vc1itg = fptoui float %vc1elv to i32

    %vc1shift = shl i32 %vc1itg,16
    %endv = or i32 %vc1shift, %vc0itg

    ret i32 %endv
}

; GLSL: uint packSnorm2x16(vec2)
define i32 @_Z13packSnorm2x16Dv2_f(<2 x float> %v) #0
{
    %v0 = extractelement <2 x float> %v, i32 0
    %v1 = extractelement <2 x float> %v, i32 1

    %vc0 = call float @llpc.fclamp.f32(float %v0, float -1.0, float 1.0)
    %vc1 = call float @llpc.fclamp.f32(float %v1, float -1.0, float 1.0)

    %vc0elv = fmul float %vc0, 32767.0
    %vc1elv = fmul float %vc1, 32767.0

    %vc0itg = fptosi float %vc0elv to i32
    %vc0itg1 = and i32 %vc0itg, 65535
    %vc1itg = fptosi float %vc1elv to i32
    %vc1shift = shl i32 %vc1itg, 16
    %endv = or i32 %vc1shift, %vc0itg1

    ret i32 %endv
}

; GLSL: uint packUnorm4x8(vec4)
define i32 @_Z12packUnorm4x8Dv4_f(<4 x float> %v) #0
{
    %v0 = extractelement <4 x float> %v, i32 0
    %v1 = extractelement <4 x float> %v, i32 1
    %v2 = extractelement <4 x float> %v, i32 2
    %v3 = extractelement <4 x float> %v, i32 3

    %vc0 = call float @llpc.fclamp.f32(float %v0, float 0.0, float 1.0)
    %vc1 = call float @llpc.fclamp.f32(float %v1, float 0.0, float 1.0)
    %vc2 = call float @llpc.fclamp.f32(float %v2, float 0.0, float 1.0)
    %vc3 = call float @llpc.fclamp.f32(float %v3, float 0.0, float 1.0)

    %vc0elv = fmul float %vc0, 255.0
    %vc1elv = fmul float %vc1, 255.0
    %vc2elv = fmul float %vc2, 255.0
    %vc3elv = fmul float %vc3, 255.0

    %1 = call i32 @llvm.amdgcn.cvt.pk.u8.f32(float %vc0elv, i32 0, i32 0)
    %2 = call i32 @llvm.amdgcn.cvt.pk.u8.f32(float %vc1elv, i32 1, i32 %1)
    %3 = call i32 @llvm.amdgcn.cvt.pk.u8.f32(float %vc2elv, i32 2, i32 %2)
    %4 = call i32 @llvm.amdgcn.cvt.pk.u8.f32(float %vc3elv, i32 3, i32 %3)

    ret i32 %4
}

; GLSL: uint packSnorm4x8(vec4)
define i32 @_Z12packSnorm4x8Dv4_f(<4 x float> %v) #0
{
    %v0 = extractelement <4 x float> %v, i32 0
    %v1 = extractelement <4 x float> %v, i32 1
    %v2 = extractelement <4 x float> %v, i32 2
    %v3 = extractelement <4 x float> %v, i32 3

    %vc0 = call float @llpc.fclamp.f32(float %v0, float -1.0, float 1.0)
    %vc1 = call float @llpc.fclamp.f32(float %v1, float -1.0, float 1.0)
    %vc2 = call float @llpc.fclamp.f32(float %v2, float -1.0, float 1.0)
    %vc3 = call float @llpc.fclamp.f32(float %v3, float -1.0, float 1.0)

    %vc0elv = fmul float %vc0, 127.0
    %vc1elv = fmul float %vc1, 127.0
    %vc2elv = fmul float %vc2, 127.0
    %vc3elv = fmul float %vc3, 127.0

    %vc0elv1 = call float @llvm.rint.f32(float %vc0elv)
    %vc1elv1 = call float @llvm.rint.f32(float %vc1elv)
    %vc2elv1 = call float @llvm.rint.f32(float %vc2elv)
    %vc3elv1 = call float @llvm.rint.f32(float %vc3elv)

    %vc0itg = fptosi float %vc0elv1 to i32
    %vc0itg1 = and i32 %vc0itg, 255
    %vc1itg = fptosi float %vc1elv1 to i32
    %vc1itg1 = and i32 %vc1itg, 255
    %vc2itg = fptosi float %vc2elv1 to i32
    %vc2itg1 = and i32 %vc2itg, 255
    %vc3itg = fptosi float %vc3elv1 to i32

    %vc1shift = shl i32 %vc1itg1, 8
    %vc2shift = shl i32 %vc2itg1, 16
    %vc3shift = shl i32 %vc3itg, 24

    %1 = or i32 %vc0itg1, %vc1shift
    %2 = or i32 %1, %vc2shift
    %3 = or i32 %2, %vc3shift

    ret i32 %3
}

; GLSL: vec2 unpackUnorm2x16(uint)
define <2 x float> @_Z15unpackUnorm2x16i(i32 %p) #0
{
    %vl = and i32 %p, 65535
    %vs = lshr i32 %p, 16
    %vlf = uitofp i32 %vl to float
    %vsf = uitofp i32 %vs to float
    %req = fdiv float 1.0, 65535.0
    %vlf0 = fmul float %vlf, %req
    %vsf0 = fmul float %vsf, %req

    %vec20 = insertelement <2 x float> undef, float %vlf0, i32 0
    %vec21 = insertelement <2 x float> %vec20, float %vsf0, i32 1

    ret <2 x float> %vec21
}

; GLSL: vec2 unpackSnorm2x16(uint)
define <2 x float> @_Z15unpackSnorm2x16i(i32 %p) #0
{
    %v0 = call i32 @llvm.amdgcn.sbfe.i32(i32 %p, i32 0, i32 16)
    %v1 = call i32 @llvm.amdgcn.sbfe.i32(i32 %p, i32 16, i32 16)

    %v00 = trunc i32 %v0 to i16
    %v10 = trunc i32 %v1 to i16

    %vl = sext i16 %v00 to i32
    %vm = sext i16 %v10 to i32

    %vlf = sitofp i32 %vl to float
    %vmf = sitofp i32 %vm to float
    %req = fdiv float 1.0, 32767.0
    %vln0 = fmul float %vlf, %req
    %vmn0 = fmul float %vmf, %req

    %vln = call float @llpc.fclamp.f32(float %vln0, float -1.0, float 1.0)
    %vmn = call float @llpc.fclamp.f32(float %vmn0, float -1.0, float 1.0)

    %vec20 = insertelement <2 x float> undef, float %vln, i32 0
    %vec21 = insertelement <2 x float> %vec20, float %vmn, i32 1

    ret <2 x float> %vec21
}

; GLSL: vec4 unpackUnorm4x8(uint)
define <4 x float> @_Z14unpackUnorm4x8i(i32 %p) #0
{
    %p0 = and i32 %p, 255
    %p10 = lshr i32 %p, 8
    %p1 = and i32 %p10, 255
    %p20 = lshr i32 %p, 16
    %p2 = and i32 %p20, 255
    %p3 = lshr i32 %p, 24

    %p0f = uitofp i32 %p0 to float
    %p1f = uitofp i32 %p1 to float
    %p2f = uitofp i32 %p2 to float
    %p3f = uitofp i32 %p3 to float

    %req = fdiv float 1.0, 255.0
    %p0u = fmul float %p0f, %req
    %p1u = fmul float %p1f, %req
    %p2u = fmul float %p2f, %req
    %p3u = fmul float %p3f, %req

    %vec40 = insertelement <4 x float> undef, float %p0u, i32 0
    %vec41 = insertelement <4 x float> %vec40, float %p1u, i32 1
    %vec42 = insertelement <4 x float> %vec41, float %p2u, i32 2
    %vec43 = insertelement <4 x float> %vec42, float %p3u, i32 3

    ret <4 x float> %vec43
}

; GLSL: vec4 unpackSnorm4x8(uint)
define <4 x float> @_Z14unpackSnorm4x8i(i32 %p) #0
{
    %v0 = call i32 @llvm.amdgcn.sbfe.i32(i32 %p, i32 0, i32 8)
    %v1 = call i32 @llvm.amdgcn.sbfe.i32(i32 %p, i32 8, i32 8)
    %v2 = call i32 @llvm.amdgcn.sbfe.i32(i32 %p, i32 16, i32 8)
    %v3 = call i32 @llvm.amdgcn.sbfe.i32(i32 %p, i32 24, i32 8)

    %v00 = trunc i32 %v0 to i8
    %v10 = trunc i32 %v1 to i8
    %v20 = trunc i32 %v2 to i8
    %v30 = trunc i32 %v3 to i8

    %p0 = sext i8 %v00 to i32
    %p1 = sext i8 %v10 to i32
    %p2 = sext i8 %v20 to i32
    %p3 = sext i8 %v30 to i32

    %p0f = sitofp i32 %p0 to float
    %p1f = sitofp i32 %p1 to float
    %p2f = sitofp i32 %p2 to float
    %p3f = sitofp i32 %p3 to float

    %req = fdiv float 1.0, 127.0
    %p00u = fmul float %p0f, %req
    %p01u = fmul float %p1f, %req
    %p02u = fmul float %p2f, %req
    %p03u = fmul float %p3f, %req

    %p0u = call float @llpc.fclamp.f32(float %p00u, float -1.0, float 1.0)
    %p1u = call float @llpc.fclamp.f32(float %p01u, float -1.0, float 1.0)
    %p2u = call float @llpc.fclamp.f32(float %p02u, float -1.0, float 1.0)
    %p3u = call float @llpc.fclamp.f32(float %p03u, float -1.0, float 1.0)

    %vec40 = insertelement <4 x float> undef, float %p0u, i32 0
    %vec41 = insertelement <4 x float> %vec40, float %p1u, i32 1
    %vec42 = insertelement <4 x float> %vec41, float %p2u, i32 2
    %vec43 = insertelement <4 x float> %vec42, float %p3u, i32 3

    ret <4 x float> %vec43
}

; GLSL: uint packHalf2x16(vec2)
define i32 @_Z12packHalf2x16Dv2_f(<2 x float> %v) #0
{
    %v0 =  extractelement <2 x float> %v, i32 0
    %v1 =  extractelement <2 x float> %v, i32 1

    %v016 = fptrunc float %v0 to half
    %v116 = fptrunc float %v1 to half

    %v0itg =  bitcast half %v016 to i16
    %v1itg =  bitcast half %v116 to i16

    %v032 = zext i16 %v0itg to i32
    %v132 = zext i16 %v1itg to i32

    %v1shf = shl i32 %v132, 16
    %v32 = or i32 %v1shf, %v032

    ret i32 %v32
}

; GLSL: vec2 unpackHalf2x16(uint)
define <2 x float> @_Z14unpackHalf2x16i(i32 %v) #0
{
    %il = trunc i32 %v to i16
    %im0 = lshr i32 %v, 16
    %im = trunc i32 %im0 to i16

    %v0 =  bitcast i16 %il to half
    %v1 =  bitcast i16 %im to half

    %v032 = fpext half %v0 to float
    %v132 = fpext half %v1 to float

    %vec20 = insertelement <2 x float> undef, float %v032, i32 0
    %vec21 = insertelement <2 x float> %vec20, float %v132, i32 1

    ret <2 x float> %vec21
}

; =====================================================================================================================
; >>>  Geometric Functions
; =====================================================================================================================

; GLSL: float length(float)
define spir_func float @_Z6lengthf(float %x) #0
{
    %end = call float @llvm.fabs.f32(float %x)
    ret float %end
}

; GLSL: float length(vec2)
define spir_func float @_Z6lengthDv2_f(<2 x float> %x) #0
{
    %x.x = extractelement <2 x float> %x, i32 0
    %x.y = extractelement <2 x float> %x, i32 1

    %v0 = fmul float %x.x, %x.x
    %v1 = fmul float %x.y, %x.y
    %sqr = fadd float %v0, %v1
    %end = call float @llvm.sqrt.f32(float %sqr)

    ret float %end
}

; GLSL: float length(vec3)
define spir_func float @_Z6lengthDv3_f(<3 x float> %x) #0
{
    %x.x = extractelement <3 x float> %x, i32 0
    %x.y = extractelement <3 x float> %x, i32 1
    %x.z = extractelement <3 x float> %x, i32 2

    %v0 = fmul float %x.x, %x.x
    %v1 = fmul float %x.y, %x.y
    %vl = fadd float %v0, %v1
    %v2 = fmul float %x.z, %x.z
    %sqr = fadd float %vl, %v2
    %end = call float @llvm.sqrt.f32(float %sqr)

    ret float %end
}

; GLSL: float length(vec4)
define spir_func float @_Z6lengthDv4_f(<4 x float> %x) #0
{
    %x.x = extractelement <4 x float> %x, i32 0
    %x.y = extractelement <4 x float> %x, i32 1
    %x.z = extractelement <4 x float> %x, i32 2
    %x.w = extractelement <4 x float> %x, i32 3

    %v0 = fmul float %x.x, %x.x
    %v1 = fmul float %x.y, %x.y
    %vl = fadd float %v0, %v1
    %v2 = fmul float %x.z, %x.z
    %v3 = fmul float %x.w, %x.w
    %vm = fadd float %v2, %v3
    %sqr = fadd float %vl, %vm
    %end = call float @llvm.sqrt.f32(float %sqr)

    ret float %end
}

; GLSL: float distance(float, float)
define spir_func float @_Z8distanceff(float %p0, float %p1) #0
{
    %subtra = fsub float %p0 ,%p1
    %1 = call float @llvm.fabs.f32(float %subtra)
    ret float %1
}

; GLSL: float distance(vec2, vec2)
define spir_func float @_Z8distanceDv2_fDv2_f(<2 x float> %p0, <2 x float> %p1) #0
{
    %subtra = fsub <2 x float> %p0 ,%p1
    %1 = call float @_Z6lengthDv2_f(<2 x float> %subtra)
    ret float %1
}

; GLSL: float distance(vec3, vec3)
define spir_func float @_Z8distanceDv3_fDv3_f(<3 x float> %p0, <3 x float> %p1) #0
{
    %subtra = fsub <3 x float> %p0 ,%p1
    %1 = call float @_Z6lengthDv3_f(<3 x float> %subtra)
    ret float %1
}

; GLSL: float distance(vec4, vec4)
define spir_func float @_Z8distanceDv4_fDv4_f(<4 x float> %p0, <4 x float> %p1) #0
{
    %subtra = fsub <4 x float> %p0 ,%p1
    %1 = call float @_Z6lengthDv4_f(<4 x float> %subtra)
    ret float %1
}
; GLSL: float dot(float, float)
define spir_func float @_Z3dotff(float %x, float %y) #0
{
    %1 = fmul float %x, %y
    ret float %1
}

; GLSL: float dot(vec2, vec2)
define spir_func float @_Z3dotDv2_fDv2_f(<2 x float> %x, <2 x float> %y) #0
{
    %x.x = extractelement <2 x float> %x, i32 0
    %x.y = extractelement <2 x float> %x, i32 1

    %y.x = extractelement <2 x float> %y, i32 0
    %y.y = extractelement <2 x float> %y, i32 1

    %v0 = fmul float %x.x, %y.x
    %v1 = fmul float %x.y, %y.y
    %end = fadd float %v0, %v1

    ret float %end
}

; GLSL: float dot(vec3, vec3)
define spir_func float @_Z3dotDv3_fDv3_f(<3 x float> %x, <3 x float> %y) #0
{
    %x.x = extractelement <3 x float> %x, i32 0
    %x.y = extractelement <3 x float> %x, i32 1
    %x.z = extractelement <3 x float> %x, i32 2

    %y.x = extractelement <3 x float> %y, i32 0
    %y.y = extractelement <3 x float> %y, i32 1
    %y.z = extractelement <3 x float> %y, i32 2

    %v0 = fmul float %x.x, %y.x
    %v1 = fmul float %x.y, %y.y
    %vl = fadd float %v0, %v1
    %v2 = fmul float %x.z, %y.z
    %end = fadd float %vl, %v2

    ret float %end
}

; GLSL: float dot(vec4, vec4)
define spir_func float @_Z3dotDv4_fDv4_f(<4 x float> %x, <4 x float> %y) #0
{
    %x.x = extractelement <4 x float> %x, i32 0
    %x.y = extractelement <4 x float> %x, i32 1
    %x.z = extractelement <4 x float> %x, i32 2
    %x.w = extractelement <4 x float> %x, i32 3

    %y.x = extractelement <4 x float> %y, i32 0
    %y.y = extractelement <4 x float> %y, i32 1
    %y.z = extractelement <4 x float> %y, i32 2
    %y.w = extractelement <4 x float> %y, i32 3

    %v0 = fmul float %x.x, %y.x
    %v1 = fmul float %x.y, %y.y
    %vl = fadd float %v0, %v1
    %v2 = fmul float %x.z, %y.z
    %v3 = fmul float %x.w, %y.w
    %vm = fadd float %v2, %v3
    %end = fadd float %vl,%vm

    ret float %end
}

; GLSL: vec3 cross(vec3, vec3)
define spir_func <3 x float> @_Z5crossDv3_fDv3_f(<3 x float> %x, <3 x float> %y) #0
{
    %x.x = extractelement <3 x float> %x, i32 0
    %x.y = extractelement <3 x float> %x, i32 1
    %x.z = extractelement <3 x float> %x, i32 2

    %y.x = extractelement <3 x float> %y, i32 0
    %y.y = extractelement <3 x float> %y, i32 1
    %y.z = extractelement <3 x float> %y, i32 2

    %l0 = fmul float %x.y, %y.z
    %l1 = fmul float %x.z, %y.x
    %l2 = fmul float %x.x, %y.y

    %r0 = fmul float %y.y, %x.z
    %r1 = fmul float %y.z, %x.x
    %r2 = fmul float %y.x, %x.y

    %1 = fsub float %l0, %r0
    %2 = fsub float %l1, %r1
    %3 = fsub float %l2, %r2

    %4 = insertelement <3 x float> undef, float %1, i32 0
    %5 = insertelement <3 x float> %4, float %2, i32 1
    %6 = insertelement <3 x float> %5, float %3, i32 2

    ret <3 x float> %6
}

; GLSL: float normalize(float)
define spir_func float @_Z9normalizef(float %x) #0
{
    %1 = fcmp ogt float %x, 0.0
    %2 = select i1 %1, float 1.0, float -1.0
    ret float %2
}

; GLSL: vec2 normalize(vec2)
define spir_func <2 x float> @_Z9normalizeDv2_f(<2 x float> %x) #0
{
    %length = call float @_Z6lengthDv2_f(<2 x float> %x)
    %rsq = fdiv float 1.0, %length

    %x.x = extractelement <2 x float> %x, i32 0
    %x.y = extractelement <2 x float> %x, i32 1

    ; We use fmul.legacy so that a zero vector is normalized to a zero vector,
    ; rather than NaNs.
    %1 = call float @llvm.amdgcn.fmul.legacy(float %x.x, float %rsq)
    %2 = call float @llvm.amdgcn.fmul.legacy(float %x.y, float %rsq)

    %3 = insertelement <2 x float> undef, float %1, i32 0
    %4 = insertelement <2 x float> %3, float %2, i32 1

    ret <2 x float> %4
}

; GLSL: vec3 normalize(vec3)
define spir_func <3 x float> @_Z9normalizeDv3_f(<3 x float> %x) #0
{
    %length = call float @_Z6lengthDv3_f(<3 x float> %x)
    %rsq = fdiv float 1.0, %length

    %x.x = extractelement <3 x float> %x, i32 0
    %x.y = extractelement <3 x float> %x, i32 1
    %x.z = extractelement <3 x float> %x, i32 2

    ; We use fmul.legacy so that a zero vector is normalized to a zero vector,
    ; rather than NaNs.
    %1 = call float @llvm.amdgcn.fmul.legacy(float %x.x, float %rsq)
    %2 = call float @llvm.amdgcn.fmul.legacy(float %x.y, float %rsq)
    %3 = call float @llvm.amdgcn.fmul.legacy(float %x.z, float %rsq)

    %4 = insertelement <3 x float> undef, float %1, i32 0
    %5 = insertelement <3 x float> %4, float %2, i32 1
    %6 = insertelement <3 x float> %5, float %3, i32 2

    ret <3 x float> %6
}

; GLSL: vec4 normalize(vec4)
define spir_func <4 x float> @_Z9normalizeDv4_f(<4 x float> %x) #0
{
    %length = call float @_Z6lengthDv4_f(<4 x float> %x)
    %rsq = fdiv float 1.0, %length

    %x.x = extractelement <4 x float> %x, i32 0
    %x.y = extractelement <4 x float> %x, i32 1
    %x.z = extractelement <4 x float> %x, i32 2
    %x.w = extractelement <4 x float> %x, i32 3

    ; We use fmul.legacy so that a zero vector is normalized to a zero vector,
    ; rather than NaNs.
    %1 = call float @llvm.amdgcn.fmul.legacy(float %x.x, float %rsq)
    %2 = call float @llvm.amdgcn.fmul.legacy(float %x.y, float %rsq)
    %3 = call float @llvm.amdgcn.fmul.legacy(float %x.z, float %rsq)
    %4 = call float @llvm.amdgcn.fmul.legacy(float %x.w, float %rsq)

    %5 = insertelement <4 x float> undef, float %1, i32 0
    %6 = insertelement <4 x float> %5, float %2, i32 1
    %7 = insertelement <4 x float> %6, float %3, i32 2
    %8 = insertelement <4 x float> %7, float %4, i32 3

    ret <4 x float> %8
}

; GLSL: float faceforward(float, float, float)
define spir_func float @_Z11faceForwardfff( float %N, float %I, float %Nref) #0
{
    %dotv = fmul float %I, %Nref
    ; Compare if dot < 0.0
    %con = fcmp olt float %dotv, 0.0

    %NN = fsub float 0.0, %N

    ; dot < 0.0, return N, otherwise return -N
    %1 = select i1 %con ,float %N, float %NN

    ret float %1
}

; GLSL: vec2 faceforward(vec2, vec2, vec2)
define spir_func <2 x float> @_Z11faceForwardDv2_fDv2_fDv2_f(<2 x float> %N, <2 x float> %I, <2 x float> %Nref) #0
{
    %dotv = call float @_Z3dotDv2_fDv2_f(<2 x float> %I, <2 x float> %Nref)
    ; Compare if dot < 0.0
    %con = fcmp olt float %dotv, 0.0

    %N.x = extractelement <2 x float> %N, i32 0
    %N.y = extractelement <2 x float> %N, i32 1

    %NN.x = fsub float 0.0, %N.x
    %NN.y = fsub float 0.0, %N.y

    ; dot < 0.0, return N, otherwise return -N
    %1 = select i1 %con ,float %N.x, float %NN.x
    %2 = select i1 %con ,float %N.y, float %NN.y

    %3 = insertelement <2 x float> undef, float %1, i32 0
    %4 = insertelement <2 x float> %3, float %2, i32 1

    ret <2 x float> %4
}

; GLSL: vec3 faceforward(vec3, vec3, vec3)
define spir_func <3 x float> @_Z11faceForwardDv3_fDv3_fDv3_f(<3 x float> %N, <3 x float> %I, <3 x float> %Nref) #0
{
    %dotv = call float @_Z3dotDv3_fDv3_f(<3 x float> %I, <3 x float> %Nref)
    ; Compare if dot < 0.0
    %con = fcmp olt float %dotv, 0.0

    %N.x = extractelement <3 x float> %N, i32 0
    %N.y = extractelement <3 x float> %N, i32 1
    %N.z = extractelement <3 x float> %N, i32 2

    %NN.x = fsub float 0.0, %N.x
    %NN.y = fsub float 0.0, %N.y
    %NN.z = fsub float 0.0, %N.z

    ; dot < 0.0, return N, otherwise return -N
    %1 = select i1 %con ,float %N.x, float %NN.x
    %2 = select i1 %con ,float %N.y, float %NN.y
    %3 = select i1 %con ,float %N.z, float %NN.z

    %4 = insertelement <3 x float> undef, float %1, i32 0
    %5 = insertelement <3 x float> %4, float %2, i32 1
    %6 = insertelement <3 x float> %5, float %3, i32 2

    ret <3 x float> %6
}

; GLSL: vec4 faceforward(vec4, vec4, vec4)
define spir_func <4 x float> @_Z11faceForwardDv4_fDv4_fDv4_f(<4 x float> %N, <4 x float> %I, <4 x float> %Nref) #0
{
    %dotv = call float @_Z3dotDv4_fDv4_f(<4 x float> %I, <4 x float> %Nref)
    ; Compare if dot < 0.0
    %con = fcmp olt float %dotv, 0.0

    %N.x = extractelement <4 x float> %N, i32 0
    %N.y = extractelement <4 x float> %N, i32 1
    %N.z = extractelement <4 x float> %N, i32 2
    %N.w = extractelement <4 x float> %N, i32 3

    %NN.x = fsub float 0.0, %N.x
    %NN.y = fsub float 0.0, %N.y
    %NN.z = fsub float 0.0, %N.z
    %NN.w = fsub float 0.0, %N.w

    ; dot < 0.0, return N, otherwise return -N
    %1 = select i1 %con ,float %N.x,  float %NN.x
    %2 = select i1 %con ,float %N.y,  float %NN.y
    %3 = select i1 %con ,float %N.z,  float %NN.z
    %4 = select i1 %con ,float %N.w,  float %NN.w

    %5 = insertelement <4 x float> undef, float %1, i32 0
    %6 = insertelement <4 x float> %5, float %2, i32 1
    %7 = insertelement <4 x float> %6, float %3, i32 2
    %8 = insertelement <4 x float> %7, float %4, i32 3

    ret <4 x float> %8
}

; GLSL: float reflect(float, float)
define spir_func float @_Z7reflectff(float %I, float %N) #0
{
    %dotin = fmul float %I, %N
    %dot = fmul float %dotin, 2.0

    ; 2 * dot(N, I) * N
    %right = fmul float %dot, %N
    %end = fsub float %I, %right

    ret float %end
}

; GLSL: vec2 reflect(vec2, vec2)
define spir_func <2 x float> @_Z7reflectDv2_fDv2_f(<2 x float> %I, <2 x float> %N) #0
{
    %dotin = call float @_Z3dotDv2_fDv2_f(<2 x float> %I, <2 x float> %N)
    %dot = fmul float %dotin, 2.0

    %1 = insertelement <2 x float> undef, float %dot, i32 0
    %dotv = insertelement <2 x float> %1, float %dot, i32 1

    ; 2 * dot(N, I) * N
    %right = fmul <2 x float> %dotv, %N
    %end = fsub <2 x float> %I, %right

    ret <2 x float> %end
}

; GLSL: vec3 reflect(vec3, vec3)
define spir_func <3 x float> @_Z7reflectDv3_fDv3_f(<3 x float> %I, <3 x float> %N) #0
{
    %dotin = call float @_Z3dotDv3_fDv3_f(<3 x float> %I, <3 x float> %N)
    %dot = fmul float %dotin, 2.0

    %1 = insertelement <3 x float> undef, float %dot, i32 0
    %2 = insertelement <3 x float> %1, float %dot, i32 1
    %dotv = insertelement <3 x float> %2, float %dot, i32 2

    ; 2 * dot(N, I) * N
    %right = fmul <3 x float> %dotv, %N
    %end = fsub <3 x float> %I, %right

    ret <3 x float> %end
}

; GLSL: vec4 reflect(vec4, vec4)
define spir_func <4 x float> @_Z7reflectDv4_fDv4_f(<4 x float> %I, <4 x float> %N) #0
{
    %dotin = call float @_Z3dotDv4_fDv4_f(<4 x float> %I, <4 x float> %N)
    %dot = fmul float %dotin, 2.0

    %1 = insertelement <4 x float> undef, float %dot, i32 0
    %2 = insertelement <4 x float> %1, float %dot, i32 1
    %3 = insertelement <4 x float> %2, float %dot, i32 2
    %dotv = insertelement <4 x float> %3, float %dot, i32 3

    ; 2 * dot(N, I) * N
    %right = fmul <4 x float> %dotv, %N
    %end = fsub <4 x float> %I, %right

    ret <4 x float> %end
}

; GLSL: float refract(float, float, float)
define spir_func float @_Z7refractfff(float %I, float %N, float %eta) #0
{
    %dotin = fmul float %I, %N
    %dotinsqr = fmul float %dotin, %dotin
    %e1 = fsub float 1.0, %dotinsqr
    %e2 = fmul float %eta, %eta
    %e3 = fmul float %e1, %e2
    %k = fsub float 1.0, %e3
    %ksqrt = call float @llvm.sqrt.f32(float %k)
    %etadot = fmul float %eta, %dotin
    %innt = fadd float %etadot, %ksqrt

    %N0 = fmul float %innt, %N
    %I0 = fmul float %I, %eta
    %S = fsub float %I0, %N0
    ; Compare k < 0
    %con = fcmp olt float %k, 0.0
    %1 = select i1 %con, float 0.0, float %S

    ret float %1
}

; GLSL: vec2 refract(vec2, vec2, float)
define spir_func <2 x float> @_Z7refractDv2_fDv2_ff(<2 x float> %I, <2 x float> %N, float %eta) #0
{
    %dotin = call float @_Z3dotDv2_fDv2_f(<2 x float> %I, <2 x float> %N)
    %dotinsqr = fmul float %dotin, %dotin
    %e1 = fsub float 1.0, %dotinsqr
    %e2 = fmul float %eta, %eta
    %e3 = fmul float %e1, %e2
    %k = fsub float 1.0, %e3
    %ksqrt = call float @llvm.sqrt.f32(float %k)
    %etadot = fmul float %eta, %dotin
    %innt = fadd float %etadot, %ksqrt

    %I.x = extractelement <2 x float> %I, i32 0
    %I.y = extractelement <2 x float> %I, i32 1

    %N.x = extractelement <2 x float> %N, i32 0
    %N.y = extractelement <2 x float> %N, i32 1

    %I0 = fmul float %I.x, %eta
    %I1 = fmul float %I.y, %eta

    %N0 = fmul float %N.x, %innt
    %N1 = fmul float %N.y, %innt

    %S0 = fsub float %I0, %N0
    %S1 = fsub float %I1, %N1

    ; Compare k < 0
    %con = fcmp olt float %k, 0.0

    %1 = select i1 %con, float 0.0, float %S0
    %2 = select i1 %con, float 0.0, float %S1

    %3 = insertelement <2 x float> undef, float %1, i32 0
    %4 = insertelement <2 x float> %3, float %2, i32 1

    ret <2 x float> %4
}

; GLSL: vec3 refract(vec3, vec3, float)
define spir_func <3 x float> @_Z7refractDv3_fDv3_ff(<3 x float> %I, <3 x float> %N, float %eta) #0
{
    %dotin = call float @_Z3dotDv3_fDv3_f(<3 x float> %I, <3 x float> %N)
    %dotinsqr = fmul float %dotin, %dotin
    %e1 = fsub float 1.0, %dotinsqr
    %e2 = fmul float %eta, %eta
    %e3 = fmul float %e1, %e2
    %k = fsub float 1.0, %e3
    %ksqrt = call float @llvm.sqrt.f32(float %k)
    %etadot = fmul float %eta, %dotin
    %innt = fadd float %etadot, %ksqrt

    %I.x = extractelement <3 x float> %I, i32 0
    %I.y = extractelement <3 x float> %I, i32 1
    %I.z = extractelement <3 x float> %I, i32 2

    %N.x = extractelement <3 x float> %N, i32 0
    %N.y = extractelement <3 x float> %N, i32 1
    %N.z = extractelement <3 x float> %N, i32 2

    %I0 = fmul float %I.x, %eta
    %I1 = fmul float %I.y, %eta
    %I2 = fmul float %I.z, %eta

    %N0 = fmul float %N.x, %innt
    %N1 = fmul float %N.y, %innt
    %N2 = fmul float %N.z, %innt

    %S0 = fsub float %I0, %N0
    %S1 = fsub float %I1, %N1
    %S2 = fsub float %I2, %N2

    ; Compare k < 0
    %con = fcmp olt float %k, 0.0

    %1 = select i1 %con, float 0.0, float %S0
    %2 = select i1 %con, float 0.0, float %S1
    %3 = select i1 %con, float 0.0, float %S2

    %4 = insertelement <3 x float> undef, float %1, i32 0
    %5 = insertelement <3 x float> %4, float %2, i32 1
    %6 = insertelement <3 x float> %5, float %3, i32 2

    ret <3 x float> %6
}

; GLSL: vec4 refract(vec4, vec4, float)
define spir_func <4 x float> @_Z7refractDv4_fDv4_ff(<4 x float> %I, <4 x float> %N, float %eta) #0
{
    %dotin = call float @_Z3dotDv4_fDv4_f(<4 x float> %I, <4 x float> %N)
    %dotinsqr = fmul float %dotin, %dotin
    %e1 = fsub float 1.0, %dotinsqr
    %e2 = fmul float %eta, %eta
    %e3 = fmul float %e1, %e2
    %k = fsub float 1.0, %e3
    %ksqrt = call float @llvm.sqrt.f32(float %k)
    %etadot = fmul float %eta, %dotin
    %innt = fadd float %etadot, %ksqrt

    %I.x = extractelement <4 x float> %I, i32 0
    %I.y = extractelement <4 x float> %I, i32 1
    %I.z = extractelement <4 x float> %I, i32 2
    %I.w = extractelement <4 x float> %I, i32 3

    %N.x = extractelement <4 x float> %N, i32 0
    %N.y = extractelement <4 x float> %N, i32 1
    %N.z = extractelement <4 x float> %N, i32 2
    %N.w = extractelement <4 x float> %N, i32 3

    %I0 = fmul float %I.x, %eta
    %I1 = fmul float %I.y, %eta
    %I2 = fmul float %I.z, %eta
    %I3 = fmul float %I.w, %eta

    %N0 = fmul float %N.x, %innt
    %N1 = fmul float %N.y, %innt
    %N2 = fmul float %N.z, %innt
    %N3 = fmul float %N.w, %innt

    %S0 = fsub float %I0, %N0
    %S1 = fsub float %I1, %N1
    %S2 = fsub float %I2, %N2
    %S3 = fsub float %I3, %N3

    ; Compare k < 0
    %con = fcmp olt float %k, 0.0

    %1 = select i1 %con, float 0.0, float %S0
    %2 = select i1 %con, float 0.0, float %S1
    %3 = select i1 %con, float 0.0, float %S2
    %4 = select i1 %con, float 0.0, float %S3

    %5 = insertelement <4 x float> undef, float %1, i32 0
    %6 = insertelement <4 x float> %5, float %2, i32 1
    %7 = insertelement <4 x float> %6, float %3, i32 2
    %8 = insertelement <4 x float> %7, float %4, i32 3

    ret <4 x float> %8
}

; =====================================================================================================================
; >>>  Integer Functions
; =====================================================================================================================

; GLSL: int/uint findLSB(int/uint)
define i32 @llpc.findIlsb.i32(i32 %value) #0
{
    %1 = call i32 @llvm.cttz.i32(i32 %value, i1 1)
    ret i32 %1
}

; GLSL: uint findMSB(uint)
define i32 @llpc.findUMsb.i32(i32 %value) #0
{
    %lz = call i32 @llvm.ctlz.i32(i32 %value, i1 1)
    %bitp = sub i32 31, %lz
    %cond = icmp eq i32 %value, 0
    %end = select i1 %cond, i32 -1, i32 %bitp
    ret i32 %end
}

; GLSL: int findMSB(int)
define i32 @llpc.findSMsb.i32(i32 %value) #0
{
    %lz = call i32 @llvm.amdgcn.sffbh.i32(i32 %value)
    %bitp = sub i32 31, %lz
    %neg1 = icmp eq i32 %value, -1
    %zero = icmp eq i32 %value, 0
    %bocon = or i1 %neg1, %zero
    %end = select i1 %bocon, i32 -1, i32 %bitp
    ret i32 %end
}

; GLSL: int bitfieldInsert(int, int, int, int)
define i32 @llpc.bitFieldInsert.i32(i32 %base, i32 %insert, i32 %offset, i32 %bits) #0
{
    %1 = shl i32 %insert, %offset
    %2 = xor i32 %1, %base
    %3 = shl i32 1, %bits
    %4 = add i32 %3, -1
    %5 = shl i32 %4, %offset
    %6 = and i32 %2, %5
    %7 = xor i32 %6, %base
    %8 = icmp eq i32 %bits, 32
    %9 = select i1 %8, i32 %insert, i32 %7
    ret i32 %9
}

; GLSL: int bitfieldExtract(int, int ,int)
define i32 @llpc.bitFieldSExtract.i32(i32 %value, i32 %offset, i32 %bits) #0
{
    %1 = icmp eq i32 %bits, 32
    %2 = call i32 @llvm.amdgcn.sbfe.i32(i32 %value, i32 %offset, i32 %bits)
    %3 = select i1 %1, i32 %value, i32 %2
    ret i32 %3
}

; GLSL: uint bitfieldExtract(uint, uint ,uint)
define i32 @llpc.bitFieldUExtract.i32(i32 %value, i32 %offset, i32 %bits) #0
{
    %1 = icmp eq i32 %bits, 32
    %2 = call i32 @llvm.amdgcn.ubfe.i32(i32 %value, i32 %offset, i32 %bits)
    %3 = select i1 %1, i32 %value, i32 %2
    %4 = icmp eq i32 %bits, 0
    %5 = select i1 %4, i32 0, i32 %3
    ret i32 %5
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
