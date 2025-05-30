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

#extension GL_ARB_sparse_texture_clamp : enable

layout(set = 0, binding = 0) uniform sampler1D          samp1D[4];
layout(set = 1, binding = 0) uniform sampler2D          samp2D;
layout(set = 2, binding = 0) uniform sampler3D          samp3D;
layout(set = 3, binding = 0) uniform samplerCube        sampCube;
layout(set = 4, binding = 0) uniform sampler1DArray     samp1DArray;
layout(set = 5, binding = 0) uniform sampler2DArray     samp2DArray;
layout(set = 6, binding = 0) uniform samplerCubeArray   sampCubeArray;

layout(set = 7, binding = 0) uniform Uniforms
{
    int   index;
    float lodClamp;
    vec4  nonConstVec1;
    vec3  nonConstVec2;
    vec3  nonConstVec3;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0);

    fragColor += textureGradClampARB(samp1D[index], nonConstVec1.x, nonConstVec2.x, nonConstVec3.x, lodClamp);

    fragColor += textureGradClampARB(samp2D, nonConstVec1.xy, nonConstVec2.xy, nonConstVec3.xy, lodClamp);

    fragColor += textureGradClampARB(samp3D, nonConstVec1.xyz, nonConstVec2.xyz, nonConstVec3.xyz, lodClamp);

    fragColor += textureGradClampARB(sampCube, nonConstVec1.xyz, nonConstVec2.xyz, nonConstVec3.xyz, lodClamp);

    fragColor += textureGradClampARB(samp1DArray, nonConstVec1.xy, nonConstVec2.x, nonConstVec3.x, lodClamp);

    fragColor += textureGradClampARB(samp2DArray, nonConstVec1.xyz, nonConstVec2.xy, nonConstVec3.xy, lodClamp);

    fragColor += textureGradClampARB(sampCubeArray, nonConstVec1.xyzw, nonConstVec2.xyz, nonConstVec3.xyz, lodClamp);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 6, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 5, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 4, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 3, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 896, {{.*}}, {{.*}}, i32 153, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 153, <2 x float> %{{[0-9]*}}, <2 x float> %{{[0-9]*}}, <2 x float> %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 2, i32 512, {{.*}}, {{.*}}, i32 153, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 3, i32 512, {{.*}}, {{.*}}, i32 153, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 153, <2 x float> %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 153, <3 x float> %{{[0-9]*}}, <2 x float> %{{[0-9]*}}, <2 x float> %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 8, i32 512, {{.*}}, {{.*}}, i32 153, <4 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 6, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 5, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 4, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 3, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 896, {{.*}}, {{.*}}, i32 153, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 153, <2 x float> %{{[0-9]*}}, <2 x float> %{{[0-9]*}}, <2 x float> %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 2, i32 512, {{.*}}, {{.*}}, i32 153, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 3, i32 512, {{.*}}, {{.*}}, i32 153, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 153, <2 x float> %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 153, <3 x float> %{{[0-9]*}}, <2 x float> %{{[0-9]*}}, <2 x float> %{{[0-9]*}}, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 8, i32 512, {{.*}}, {{.*}}, i32 153, <4 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, <3 x float> %{{[0-9]*}}, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call <8 x i32> @llvm.amdgcn.readfirstlane.v8i32
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call <4 x i32> @llvm.amdgcn.readfirstlane.v4i32
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.1d.v4f32.f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}({{.*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.2d.v4f32.f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}({{.*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, {{.*}})
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.3d.v4f32.f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}({{.*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, {{.*}})
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubesc(float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}})
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubetc(float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}})
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubema(float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}})
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubeid(float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}})
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.cube.v4f32.f32.f32
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.1darray.v4f32.f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}({{.*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[.i0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.2darray.v4f32.f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}({{.*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.cube.v4f32.f32.f32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
