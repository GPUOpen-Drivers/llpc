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
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: float @spirv.image.sample.f32.Cube.dref

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @"llpc.call.get.sampler.desc.ptr
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr
; SHADERTEST: call float @llpc.image.sample.f32.Cube.dref

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call float @llvm.amdgcn.cubesc
; SHADERTEST: call float @llvm.amdgcn.cubetc
; SHADERTEST: call float @llvm.amdgcn.cubema
; SHADERTEST: call float @llvm.amdgcn.cubeid
; SHADERTEST: call float @llvm.amdgcn.image.sample.c.cube.f32.f32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
