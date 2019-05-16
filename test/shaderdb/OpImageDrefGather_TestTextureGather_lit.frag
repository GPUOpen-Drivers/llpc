#version 450

layout(set = 0, binding = 0) uniform sampler2DShadow      samp2DShadow;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow samp2DArrayShadow[4];
layout(set = 0, binding = 1) uniform sampler2DRectShadow  samp2DRectShadow;

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = textureGather(samp2DShadow, vec2(0.1), 0.9);
    f4 += textureGather(samp2DArrayShadow[index], vec3(0.2), 0.8);
    f4 += textureGather(samp2DRectShadow, vec2(1.0), 0.7);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.gather.f32.2D.dref{{.*}}({{.*}},{{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 0x3FECCCCCC0000000,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 1, i32 0,{{.*}}, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 1, i32 0,{{.*}}, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.gather.f32.2DArray.dref{{.*}}({{.*}},{{.*}}, <3 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000>, float 0x3FE99999A0000000,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.gather.f32.Rect.dref{{.*}}({{.*}},{{.*}}, <2 x float> <float 1.000000e+00, float 1.000000e+00>, float 0x3FE6666660000000,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.c.lz.2d.v4f32.f32(i32 1, float 0x3FECCCCCC0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.c.lz.2darray.v4f32.f32(i32 1, float 0x3FE99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.c.lz.2d.v4f32.f32(i32 1, float 0x3FE6666660000000, float 1.000000e+00, float 1.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
