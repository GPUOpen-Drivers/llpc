#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(set = 0, binding = 0) uniform samplerCube samp;
layout(location = 0) in vec3 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = texture(samp, inUV);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 3, i32 512, {{.*}}, {{.*}}, i32 1, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 3, i32 512, {{.*}}, {{.*}}, i32 1, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubesc
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubetc
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubema
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubeid
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.cube.v4f32.f32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
