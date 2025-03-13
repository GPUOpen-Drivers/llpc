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


layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput    subIn1;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInputMS  subIn2;
layout(input_attachment_index = 2, set = 0, binding = 2) uniform isubpassInput   subIn3;
layout(input_attachment_index = 3, set = 0, binding = 3) uniform isubpassInputMS subIn4;
layout(input_attachment_index = 4, set = 0, binding = 4) uniform usubpassInput   subIn5;
layout(input_attachment_index = 5, set = 0, binding = 5) uniform usubpassInputMS subIn6;

layout(location = 0) out vec4  fsOut1;
layout(location = 1) out ivec4 fsOut2;
layout(location = 2) out uvec4 fsOut3;

void main()
{
    fsOut1  = subpassLoad(subIn1);
    fsOut1 += subpassLoad(subIn2, 7);
    fsOut2  = subpassLoad(subIn3);
    fsOut2 += subpassLoad(subIn4, 7);
    fsOut3  = subpassLoad(subIn5);
    fsOut3 += subpassLoad(subIn6, 7);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 5)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 4)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 3)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.load.v4f32(i32 1, i32 608, {{.*}}, <2 x i32> zeroinitializer)
; SHADERTEST: call {{.*}} @lgc.create.image.load.with.fmask.v4f32(i32 6, i32 608, {{.*}}, {{.*}}, <2 x i32> zeroinitializer, i32 7)
; SHADERTEST: call {{.*}} @lgc.create.image.load.v4i32(i32 1, i32 612, {{.*}}, <2 x i32> zeroinitializer)
; SHADERTEST: call {{.*}} @lgc.create.image.load.with.fmask.v4i32(i32 6, i32 612, {{.*}}, {{.*}}, <2 x i32> zeroinitializer, i32 7)
; SHADERTEST: call {{.*}} @lgc.create.image.load.v4i32(i32 1, i32 608, {{.*}}, <2 x i32> zeroinitializer)
; SHADERTEST: call {{.*}} @lgc.create.image.load.with.fmask.v4i32(i32 6, i32 608, {{.*}}, {{.*}}, <2 x i32> zeroinitializer, i32 7)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i32{{(\.v8i32)?}}(i32 15,{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32{{(\.v8i32)?}}(i32 15, {{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32{{(\.v8i32)?}}(i32 15,{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
