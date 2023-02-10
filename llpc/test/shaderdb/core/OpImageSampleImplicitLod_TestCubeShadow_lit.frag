#version 450 core

layout(set = 0, binding = 0) uniform samplerCubeShadow samp2DS;
layout(location = 0) in vec4 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = vec4(texture(samp2DS, inUV));
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.f32(i32 3, i32 512, {{.*}}, {{.*}}, i32 513, {{.*}}, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.f32(i32 3, i32 512, {{.*}}, {{.*}}, i32 513, {{.*}}, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubesc
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubetc
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubema
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubeid
; SHADERTEST: call {{.*}} float @llvm.amdgcn.image.sample.c.cube.f32.f32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
