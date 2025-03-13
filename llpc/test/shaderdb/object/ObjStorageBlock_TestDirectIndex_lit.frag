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


layout(std430, column_major, set = 0, binding = 1) buffer BufferObject
{
    uint ui;
    vec4 v4;
    vec4 v4_array[2];
} ssbo[2];

layout(location = 0) out vec4 output0;

void main()
{
    output0 = ssbo[0].v4_array[1];

    ssbo[1].ui = 0;
    ssbo[1].v4 = vec4(3);
    ssbo[1].v4_array[0] = vec4(4);
    ssbo[1].v4_array[1] = vec4(5);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 0, <4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1077936128, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1077936128, <4 x i32> {{%[^,]+}}, i32 20, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1077936128, <4 x i32> {{%[^,]+}}, i32 24, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1077936128, <4 x i32> {{%[^,]+}}, i32 28, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1082130432, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1082130432, <4 x i32> {{%[^,]+}}, i32 36, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1082130432, <4 x i32> {{%[^,]+}}, i32 40, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1082130432, <4 x i32> {{%[^,]+}}, i32 44, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1084227584, <4 x i32> {{%[^,]+}}, i32 48, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1084227584, <4 x i32> {{%[^,]+}}, i32 52, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1084227584, <4 x i32> {{%[^,]+}}, i32 56, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1084227584, <4 x i32> {{%[^,]+}}, i32 60, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
