#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(set = 0, binding = 0) uniform sampler1D  samp1D;
layout(set = 1, binding = 0) uniform sampler2D  samp2D[4];

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = textureLod(samp1D, 0.5, 0.4);
    f4 += textureLod(samp2D[index], vec2(0.6), 0.7);

    fragColor = f4;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 512, {{.*}}, {{.*}}, i32 33, float 5.000000e-01, float 0x3FD99999A0000000)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 896, {{.*}}, {{.*}}, i32 33, <2 x float> {{(splat \(float 0x3FE3333340000000\))|(<float 0x3FE3333340000000, float 0x3FE3333340000000>)}}, float 0x3FE6666660000000)

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 512, {{.*}}, {{.*}}, i32 33, float 5.000000e-01, float 0x3FD99999A0000000)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 896, {{.*}}, {{.*}}, i32 33, <2 x float> {{(splat \(float 0x3FE3333340000000\))|(<float 0x3FE3333340000000, float 0x3FE3333340000000>)}}, float 0x3FE6666660000000)

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.l.1d.v4f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}({{.*}}, float 5.000000e-01, float 0x3FD99999A0000000, {{.*}})
; SHADERTEST: call <8 x i32> @llvm.amdgcn.readfirstlane.v8i32
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.l.2d.v4f32.f32{{(\.v8i32)?}}{{(\.v4i32)?}}({{.*}}, float 0x3FE3333340000000, float 0x3FE3333340000000, float 0x3FE6666660000000, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
