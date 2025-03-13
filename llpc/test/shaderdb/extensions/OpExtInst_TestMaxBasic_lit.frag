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
    vec4 fmax = max(a,b);
    ivec4 imax = max(ivec4(a), ivec4(b));
    uvec4 umax = max(uvec4(a), uvec4(b));
    frag_color = fmax + vec4(imax) + vec4(umax);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.maxnum.v4f32(<4 x float>
; SHADERTEST: = call <4 x i32> @llvm.smax.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}})
; SHADERTEST: = call <4 x i32> @llvm.umax.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
