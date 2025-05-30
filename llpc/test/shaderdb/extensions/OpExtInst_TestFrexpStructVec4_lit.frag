#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/


layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;
layout(location = 0) out vec4 frag_color;

void main()
{
    // FrexpStruct
    ivec4 fo = ivec4(0);
    vec4 fv = frexp(a, fo);
    frag_color = vec4(fv * fo);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call <4 x float> (...) @lgc.create.extract.significand.v4f32(<4 x float>
; SHADERTEST: = call <4 x i32> (...) @lgc.create.extract.exponent.v4i32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST-DAG: = call float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST-DAG: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST-DAG: = call float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST-DAG: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST-DAG: = call float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST-DAG: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST-DAG: = call float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST-DAG: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
