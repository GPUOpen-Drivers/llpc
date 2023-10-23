#version 450 core

layout(set = 0, binding = 0) uniform texture2D      tex2D;
layout(set = 0, binding = 1) uniform samplerShadow  sampShadow;

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(textureLod(sampler2DShadow(tex2D, sampShadow), vec3(0.0), 0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.image.sample.f32(i32 1, i32 512, <8 x i32>

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call {{.*}} float @llvm.amdgcn.image.sample.c.lz.2d.f32.f16(i32 1, float 0.000000e+00, half 0xH0000, half 0xH0000, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
