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


layout(set = 0, binding = 0, rgba32f) uniform image1D           img1D;
layout(set = 0, binding = 1, rgba32f) uniform image2DRect       img2DRect;
layout(set = 1, binding = 0, rgba32f) uniform imageBuffer       imgBuffer[4];
layout(set = 2, binding = 0, rgba32f) uniform imageCubeArray    imgCubeArray[4];
layout(set = 0, binding = 2, rgba32f) uniform image2DMS         img2DMS;

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = vec4(0.0);
    f4 += imageLoad(img1D, 1);
    f4 += imageLoad(img2DRect, ivec2(2, 3));
    f4 += imageLoad(imgBuffer[index], 4);
    f4 += imageLoad(imgCubeArray[index + 1], ivec3(5, 6, 7));
    f4 += imageLoad(img2DMS, ivec2(8, 9), 2);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.load.v4f32(i32 0, i32 512, {{.*}}, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.image.load.v4f32(i32 9, i32 512, {{.*}}, <2 x i32> <i32 2, i32 3>)
; SHADERTEST: call {{.*}} @lgc.create.image.load.v4f32(i32 10, i32 640, {{.*}}, i32 4)
; SHADERTEST: call {{.*}} @lgc.create.image.load.v4f32(i32 8, i32 640, {{.*}}, <4 x i32> <i32 5, i32 6, i32 1, i32 1>)
; SHADERTEST: call {{.*}} @lgc.create.image.load.v4f32(i32 6, i32 512, {{.*}}, <3 x i32> <i32 8, i32 9, i32 2>)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.1d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 2, i16 3, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.struct.buffer.load.format.v4f32{{(\.v4i32)?}}({{.*}}, i32 4, i32 0, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.cube.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 5, i16 6, i16 7, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 8, i16 9, i16 2, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
