#version 450 core

layout(set = 0, binding = 0) uniform sampler2DShadow samp2DS;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = vec4(textureGather(samp2DS, inUV, 2));
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: [[IMAGEPTR:%[0-9A-Za-z_.-]+]] = call {{.*}} @"llpc.call.get.image.desc.ptr{{.*}}(i32 0, i32 0
; SHADERTEST: [[SAMPLERPTR:%[0-9A-Za-z_.-]+]] = call {{.*}} @"llpc.call.get.sampler.desc.ptr{{.*}}(i32 0, i32 0
; SHADERTEST: [[SAMPLER:%[0-9A-Za-z_.-]+]] = call <4 x i32> (...) @llpc.call.load.desc.from.ptr.v4i32({ <4 x i32> addrspace(4)*, i32 } [[SAMPLERPTR]])
; SHADERTEST: [[IMAGE:%[0-9A-Za-z_.-]+]] = call <8 x i32> (...) @llpc.call.load.desc.from.ptr.v8i32({ <8 x i32> addrspace(4)*, i32 } [[IMAGEPTR]])
; SHADERTEST: call reassoc nnan nsz arcp contract <4 x float> {{.*}}@llpc.call.image.gather.v4f32(i32 1, i32 0, <8 x i32> [[IMAGE]], <4 x i32> [[SAMPLER]],{{.*}},{{.*}} float 2.000000e+00

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.l.2d.v4f32.f32(i32 1, float 2.000000e+00,
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
