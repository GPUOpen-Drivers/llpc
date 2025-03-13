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


struct AggregateType
{
    vec4    v1;
    float   v2;
    vec2    v3;
    mat2    v4;
    vec4    v5[2];
};

layout(set = 0, binding = 0) uniform UBO
{
    AggregateType m1;
};

layout(set = 1, binding = 0) uniform Uniforms
{
    AggregateType c1;
    int i;
};

AggregateType temp1;
AggregateType temp2;

layout(location = 0) out vec4 o1;
layout(location = 1) out vec4 o2;
layout(location = 2) out vec4 o3;

void main()
{
    temp1 = m1;
    temp2 = c1;
    o1 = temp1.v5[i];
    o2 = m1.v5[i];
    o3 = c1.v1;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: load

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: load <4 x float>,
; SHADERTEST: load i32,

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
