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
    vec4 fv = pow(a, b);
    fv.x = pow(2.0, 3.0) + pow(-2.0, 2.0);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.power.v4f32(<4 x float>
; SHADERTEST: store float 1.200000e+01, ptr addrspace(5) %{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.pow.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: [[mul1:%.i[0-9]*]] = call reassoc nnan nsz arcp contract afn float @llvm.pow.f32(float
; SHADERTEST: [[mul2:%.i[0-9]*]] = call reassoc nnan nsz arcp contract afn float @llvm.pow.f32(float
; SHADERTEST: [[mul3:%.i[0-9]*]] = call reassoc nnan nsz arcp contract afn float @llvm.pow.f32(float
; SHADERTEST-NOT: = call reassoc nnan nsz arcp contract afn float @llvm.pow.f32(float
; SHADERTEST: float 1.200000e+01, float [[mul1]], float [[mul2]], float [[mul3]]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
