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
layout(set = 0, binding = 1) uniform sampler3D samp3D;
layout(set = 0, binding = 2) uniform sampler2DRect samp2DRect;
layout(set = 0, binding = 3) uniform sampler1DArray samp1DArray;
layout(set = 2, binding = 0) uniform sampler2DArray samp2DArray[4];

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = texelFetchOffset(samp1D, 2, 3, 4);
    f4 += texelFetchOffset(samp2D[index], ivec2(5), 6, ivec2(7));
    f4 += texelFetchOffset(samp3D, ivec3(1), 2, ivec3(3));
    f4 += texelFetchOffset(samp2DRect, ivec2(4), ivec2(5));
    f4 += texelFetchOffset(samp1DArray, ivec2(5), 6, 7);
    f4 += texelFetchOffset(samp2DArray[index], ivec3(1), 2, ivec2(3));

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// GLSL}} program compile/link log
; SHADERTEST: {{.*}} OpImageFetch {{.*}} Lod|ConstOffset {{.*}}
; SHADERTEST: {{.*}} OpImageFetch {{.*}} Lod|ConstOffset {{.*}}
; SHADERTEST: {{.*}} OpImageFetch {{.*}} Lod|ConstOffset {{.*}}
; SHADERTEST: {{.*}} OpImageFetch {{.*}} ConstOffset {{.*}}
; SHADERTEST: {{.*}} OpImageFetch {{.*}} Lod|ConstOffset {{.*}}
; SHADERTEST: {{.*}} OpImageFetch {{.*}} Lod|ConstOffset {{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 2, i32 0
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 3
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 2
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 1, i32 0
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 0, i32 1536, {{.*}}, i32 6, i32 3)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 1664, {{.*}}, <2 x i32> {{(splat \(i32 12\))|(<i32 12, i32 12>)}}, i32 6)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 2, i32 1536, {{.*}}, <3 x i32> {{(splat \(i32 4\))|(<i32 4, i32 4, i32 4>)}}, i32 2)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 9, i32 1536, {{.*}}, <2 x i32> {{(splat \(i32 9\))|(<i32 9, i32 9>)}})
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 4, i32 1536, {{.*}}, <2 x i32> <i32 12, i32 5>, i32 6)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 5, i32 1664, {{.*}}, <3 x i32> <i32 4, i32 4, i32 1>, i32 2)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.1d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 6, i16 3, <8 x i32> %{{.*}}, i32 0, i32 0), !invariant.load !{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.2d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 12, i16 12, i16 6, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.3d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 4, i16 4, i16 4, i16 2, <8 x i32> %{{.*}}, i32 0, i32 0), !invariant.load !{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 9, i16 9, <8 x i32> %{{.*}}, i32 0, i32 0), !invariant.load !{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.1darray.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 12, i16 5, i16 6, <8 x i32> %{{.*}}, i32 0, i32 0), !invariant.load !{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.2darray.v4f32.i16{{(\.v8i32)?}}(i32 15, i16 4, i16 4, i16 1, i16 2, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
