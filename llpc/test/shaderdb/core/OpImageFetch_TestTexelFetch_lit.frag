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


layout(set = 0, binding = 0) uniform sampler1D samp1D;
layout(set = 1, binding = 0) uniform sampler2D samp2D[4];
layout(set = 0, binding = 1) uniform sampler2DRect samp2DRect;
layout(set = 0, binding = 2) uniform samplerBuffer sampBuffer;
layout(set = 0, binding = 3) uniform sampler2DMS samp2DMS[4];

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = texelFetch(samp1D, 2, 2);
    f4 += texelFetch(samp2D[index], ivec2(7), 8);
    f4 += texelFetch(samp2DRect, ivec2(3));
    f4 += texelFetch(sampBuffer, 5);
    f4 += texelFetch(samp2DMS[index], ivec2(6), 4);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 3
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 4, i32 4, i64 0, i32 2
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 1, i32 0
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 0, i32 1536, {{.*}}, i32 2, i32 2)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 1664, {{.*}}, <2 x i32> {{(splat \(i32 7\))|(<i32 7, i32 7>)}}, i32 8)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 9, i32 1536, {{.*}}, <2 x i32> {{(splat \(i32 3\))|(<i32 3, i32 3>)}})
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 10, i32 1536, {{.*}}, i32 5)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.with.fmask.v4f32(i32 6, i32 1664, {{.*}}, {{.*}}, <2 x i32> {{(splat \(i32 6\))|(<i32 6, i32 6>)}}, i32 4)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.1d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 2, i16 2, <8 x i32> %{{.*}}, i32 0, i32 0), !invariant.load !{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.2d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 7, i16 7, i16 8, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 3, i16 3, <8 x i32> %{{.*}}, i32 0, i32 0), !invariant.load !{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.struct.buffer.load.format.v4f32{{(\.v4i32)?}}({{.*}}, i32 5, i32 0, i32 0, i32 0), !invariant.load
; SHADERTEST: call i32 @llvm.amdgcn.image.load.2d.i32.i16{{(\.v8i32)?}}(i32 1, i16 6, i16 6, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32{{(\.v8i32)?}}(i32 15, i32 6, i32 6,{{.*}},{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
