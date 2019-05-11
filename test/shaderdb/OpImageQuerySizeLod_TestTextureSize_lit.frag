#version 450

layout(set = 0, binding = 0) uniform sampler2D            samp2D[4];
layout(set = 0, binding = 4) uniform sampler2DShadow      samp2DShadow;
layout(set = 0, binding = 5) uniform sampler3D            samp3D;

layout(set = 1, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 2) out ivec3 i3;

void main()
{
    i3.xy  = textureSize(samp2D[index], 3);
    i3.xy += textureSize(samp2DShadow, 4);
    i3    += textureSize(samp3D, 5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 0,{{.*}}, i1 false)
; SHADERTEST: call <2 x i32> @llpc.image.querynonlod.sizelod.2D.v2i32{{.*}}({{.*}}, i32 3,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 4, i32 0, i1 false)
; SHADERTEST: call <2 x i32> @llpc.image.querynonlod.sizelod.2D.v2i32{{.*}}({{.*}}, i32 4,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 5, i32 0, i1 false)
; SHADERTEST: call <3 x i32> @llpc.image.querynonlod.sizelod.3D.v3i32{{.*}}({{.*}}, i32 5,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getresinfo.2d.v2f32.i32(i32 3, i32 3,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getresinfo.2d.v2f32.i32(i32 3, i32 4,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <3 x float> @llvm.amdgcn.image.getresinfo.3d.v3f32.i32(i32 7, i32 5,{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
