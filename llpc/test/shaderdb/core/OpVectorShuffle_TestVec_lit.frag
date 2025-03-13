#version 450
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


layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform Uniforms
{
    vec3 color;
};

void main()
{
    vec4 data  = vec4(0.5);

    data.xw = color.zy;

    fragColor = data;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{.*}} = extractelement <2 x float> %{{.*}}, i64 0
; SHADERTEST: %{{.*}} = extractelement <2 x float> %{{.*}}, i64 1
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: [[VEC1:%.*]] = shufflevector <3 x float> %{{.*}}, <3 x float> {{undef|poison}}, <4 x i32> <i32 {{undef|poison}}, i32 1, i32 2, i32 {{undef|poison}}>
; SHADERTEST: [[VEC2:%.*]] = shufflevector <4 x float> <float {{undef|poison}}, float 5.000000e-01, float 5.000000e-01, float {{undef|poison}}>, <4 x float> [[VEC1]], <4 x i32> <i32 6, i32 1, i32 2, i32 5>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
