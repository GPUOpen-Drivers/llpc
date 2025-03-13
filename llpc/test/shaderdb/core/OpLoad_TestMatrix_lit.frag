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


layout(set  = 0, binding = 0) uniform Uniforms
{
    mat3 m;
    int index;
};

layout(set = 1, binding = 1) uniform BB
{
    mat3 m2;
    layout (row_major) mat2x3 m3;
};

layout(location = 0) out vec3 o1;
layout(location = 1) out vec3 o2;
layout(location = 2) out vec3 o3;

void main()
{
    o1 = m[2];
    o2 = m2[2] + m3[1];
    o3 = m2[index] + m3[index];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: load <3 x float>, ptr

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: load <3 x float>, ptr

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
