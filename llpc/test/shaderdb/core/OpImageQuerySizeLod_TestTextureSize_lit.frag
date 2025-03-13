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


layout(set = 0, binding = 0) uniform sampler2D            samp2D[4];
layout(set = 0, binding = 4) uniform sampler2DShadow      samp2DShadow;
layout(set = 0, binding = 5) uniform sampler3D            samp3D;

layout(set = 1, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 2) out ivec3 i3;

void main()
{
    i3 = ivec3(0);
    i3.xy += textureSize(samp2D[index], 3);
    i3.xy += textureSize(samp2DShadow, 4);
    i3    += textureSize(samp3D, 5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 5)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 4)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 640, {{.*}}, i32 3)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 512, {{.*}}, i32 4)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 2, i32 512, {{.*}}, i32 5)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
