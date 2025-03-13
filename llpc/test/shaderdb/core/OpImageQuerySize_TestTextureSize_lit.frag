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


layout(set = 0, binding = 0) uniform sampler2DRect       samp2DRect;
layout(set = 1, binding = 0) uniform samplerBuffer       samp2DBuffer[4];
layout(set = 0, binding = 1) uniform sampler2DMS         samp2DMS;
layout(set = 2, binding = 0) uniform sampler2DMSArray    samp2DMSArray[4];

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out ivec3 i3;

void main()
{
    i3 = ivec3(0);
    i3.xy += textureSize(samp2DRect);
    i3.x  += textureSize(samp2DBuffer[index]);
    i3.xy += textureSize(samp2DMS);
    i3    += textureSize(samp2DMSArray[index]);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 9, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.i32(i32 10, i32 640, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 6, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 7, i32 640, {{.*}}, i32 0)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
