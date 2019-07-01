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
; SHADERTEST: [[SAMPLERPTR:%[0-9A-Za-z_.-]+]] = call {{.*}} @"llpc.call.get.sampler.desc.ptr{{.*}}(i32 0, i32 0
; SHADERTEST: [[SAMPLERPTR2:%[0-9A-Za-z_.-]+]] = call {{.*}} @"llpc.call.index.desc.ptr.s[p4v4i32,i32]"({ <4 x i32> addrspace(4)*, i32 } [[SAMPLERPTR]],
; SHADERTEST: [[SAMPLER:%[0-9A-Za-z_.-]+]] = call <4 x i32> (...) @llpc.call.load.desc.from.ptr.v4i32({ <4 x i32> addrspace(4)*, i32 } [[SAMPLERPTR2]])
; SHADERTEST: [[IMAGEPTR:%[0-9A-Za-z_.-]+]] = call {{.*}} @"llpc.call.get.image.desc.ptr{{.*}}(i32 0, i32 0
; SHADERTEST: [[IMAGEPTR2:%[0-9A-Za-z_.-]+]] = call {{.*}} @"llpc.call.index.desc.ptr.s[p4v8i32,i32]"({ <8 x i32> addrspace(4)*, i32 } [[IMAGEPTR]],
; SHADERTEST: [[IMAGE:%[0-9A-Za-z_.-]+]] = call <8 x i32> (...) @llpc.call.load.desc.from.ptr.v8i32({ <8 x i32> addrspace(4)*, i32 } [[IMAGEPTR2]])
; SHADERTEST: call <4 x float> @llpc.image.gather.f32.2D.dref{{.*}}(<4 x i32> [[SAMPLER]], <8 x i32> [[IMAGE]],{{.*}}, float 2.000000e+00,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.c.lz.2d.v4f32.f32(i32 1, float 2.000000e+00,{{.*}},{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
