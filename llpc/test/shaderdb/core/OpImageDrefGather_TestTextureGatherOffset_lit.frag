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


layout(set = 0, binding = 0) uniform sampler2DShadow      samp2DShadow;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow samp2DArrayShadow[4];
layout(set = 0, binding = 1) uniform sampler2DRectShadow  samp2DRectShadow;

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
    ivec2 i2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = textureGatherOffset(samp2DShadow, vec2(0.1), 0.9, ivec2(1));
    f4 += textureGatherOffset(samp2DArrayShadow[index], vec3(0.2), 0.8, i2);
    f4 += textureGatherOffset(samp2DRectShadow, vec2(1.0), 0.7, i2);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4(i32 2, i32 2, i64 0, i32 0)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.gather.v4f32(i32 1, i32 512, ptr addrspace(4)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.gather.v4f32(i32 5, i32 896, ptr addrspace(4)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.gather.v4f32(i32 9, i32 512, ptr addrspace(4)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 257, float 0x3FECCCCCC0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, <8 x i32> %{{[-0-9A-Za0z_.]+}}, <4 x i32> %{{[-0-9A-Za0z_.]+}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2darray.v4f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1,
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1,
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
