#version 450

layout(set = 0, binding = 0) uniform sampler2DShadow      samp2DShadow;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow samp2DArrayShadow[4];
layout(set = 0, binding = 1) uniform sampler2DRectShadow  samp2DRectShadow;

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
    ivec2 i2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = textureGatherOffset(samp2DShadow, vec2(0.1), 0.9, ivec2(1));
    f4 += textureGatherOffset(samp2DArrayShadow[index], vec3(0.2), 0.8, i2);
    f4 += textureGatherOffset(samp2DRectShadow, vec2(1.0), 0.7, i2);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 0, i32 0)
; SHADERTEST: call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.image.gather.v4f32(i32 1, i32 0, <8 x i32>
; SHADERTEST: call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.image.gather.v4f32(i32 5, i32 0, <8 x i32>
; SHADERTEST: call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.image.gather.v4f32(i32 1, i32 0, <8 x i32>

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.l.o.2d.v4f32.f32(i32 1, i32 257, float 0x3FECCCCCC0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0.000000e+00, <8 x i32> %{{[-0-9A-Za0z_.]+}}, <4 x i32> %{{[-0-9A-Za0z_.]+}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.l.o.2darray.v4f32.f32(i32 1,
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.l.o.2d.v4f32.f32(i32 1,
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
