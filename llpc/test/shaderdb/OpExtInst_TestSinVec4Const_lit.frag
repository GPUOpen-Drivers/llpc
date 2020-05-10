#version 450 core

layout(location = 0) in vec4 a;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = sin(a);
    fv.x = sin(1.5);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x float> @llvm.sin.v4f32(
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x float> @llvm.sin.v4f32(
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.sin.f32(float %{{.*}})
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.sin.f32(float %{{.*}})
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.sin.f32(float %{{.*}})
; SHADERTEST-NOT: = call{{.*}} float @llvm.sin.f32(float %{{.*}})
; SHADERTEST: ret void
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
