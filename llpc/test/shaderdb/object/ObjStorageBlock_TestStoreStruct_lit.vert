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


struct S
{
    int  i;
    vec4 f4;
    mat4 m4;
};

layout(std430, binding = 0) buffer Block
{
    S    s0;
    S    s1;
} block;

void main()
{
    block.s1 = block.s0;

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 96, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 112, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 128, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 144, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 160, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 176, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
