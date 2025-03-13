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


layout(std430, binding = 0) buffer Block
{
    double d1;
    dvec2  d2;
    dvec3  d3;
    dvec4  d4;
} block;

void main()
{
    block.d1 += 1.0;
    block.d2 += dvec2(2.0);
    block.d3 += dvec3(3.0);
    block.d4 += dvec4(4.0);

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2i32{{(\.v4i32)?}}(<2 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 48, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2i32{{(\.v4i32)?}}(<2 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 48, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 64, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 80, i32 0, i32 0)
; SHADERTEST: shufflevector <8 x i32> {{%[^,]+}}, <8 x i32> {{poison|undef}}, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
; SHADERTEST: shufflevector <8 x i32> {{%[^,]+}}, <8 x i32> {{poison|undef}}, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 64, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 80, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
