#version 450

layout(set = 0, binding = 0) uniform sampler1D          samp1D;
layout(set = 1, binding = 0) uniform sampler2D          samp2D[4];
layout(set = 0, binding = 1) uniform sampler2DShadow    samp2DShadow;
layout(set = 2, binding = 0) uniform samplerCubeArray   sampCubeArray[4];

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1 = textureQueryLevels(samp1D);
    i1 += textureQueryLevels(samp2D[index]);
    i1 += textureQueryLevels(samp2DShadow);
    i1 += textureQueryLevels(sampCubeArray[index]);

    fragColor = vec4(i1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.levels.1D{{.*}}({{.*}},{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 1, i32 0,{{.*}}, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.levels.2D{{.*}}({{.*}},{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.levels.2D{{.*}}({{.*}},{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 2, i32 0,{{.*}}, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.levels.CubeArray{{.*}}({{.*}},{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call float @llvm.amdgcn.image.getresinfo.1d.f32.i32(i32 8, i32 undef,{{.*}}, i32 0, i32 0)
; SHADERTEST: call float @llvm.amdgcn.image.getresinfo.2d.f32.i32(i32 8, i32 undef,{{.*}}, i32 0, i32 0)
; SHADERTEST: call float @llvm.amdgcn.image.getresinfo.2d.f32.i32(i32 8, i32 undef,{{.*}}, i32 0, i32 0)
; SHADERTEST: call float @llvm.amdgcn.image.getresinfo.cube.f32.i32(i32 8, i32 undef,{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
