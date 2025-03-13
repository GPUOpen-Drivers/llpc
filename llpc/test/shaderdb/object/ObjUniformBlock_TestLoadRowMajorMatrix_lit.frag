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


layout(std140, binding = 0, row_major) uniform Block
{
    vec3   f;
    mat2x3 m2x3;
} block;

layout(location = 0) out vec3 f;

void main()
{
    mat2x3 m2x3 = block.m2x3;
    f = m2x3[1] + block.f;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 20, i32 0)
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 36, i32 0)
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 52, i32 0)
; SHADERTEST-DAG: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 8, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
