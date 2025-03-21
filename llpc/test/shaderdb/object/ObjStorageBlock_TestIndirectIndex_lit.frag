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


layout(std430, column_major, set = 0, binding = 0) buffer BufferObject
{
    uint ui;
    vec4 v4;
    vec4 v4_array[2];
}ssbo[2];

layout(set = 1, binding = 0) uniform Uniforms
{
    int i;
    int j;
};

layout(location = 0) out vec4 output0;

void main()
{
    output0 = ssbo[i].v4_array[j];

    ssbo[i].ui = 0;
    ssbo[i].v4 = vec4(3);
    ssbo[i].v4_array[j] = vec4(4);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = getelementptr [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>], ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 %{{[0-9]*}}, i32 3, i32 %{{[0-9]*}}
; SHADERTEST: %{{[0-9]*}} = getelementptr [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>], ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 %{{[0-9]*}}, i32 0
; SHADERTEST: %{{[0-9]*}} = getelementptr [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>], ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 %{{[0-9]*}}, i32 2
; SHADERTEST: %{{[0-9]*}} = getelementptr [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>], ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 %{{[0-9]*}}, i32 3, i32 %{{[0-9]*}}

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-COUNT-3: call ptr addrspace(7) @lgc.load.buffer.desc(i64 0, i32 0, i32 %{{[0-9]*}},

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
