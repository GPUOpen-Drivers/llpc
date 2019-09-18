#version 450 core

layout(location = 0) in vec4 a;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = sqrt(a);
    fv.x = sqrt(1.5);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call <4 x float> @llvm.sqrt.v4f32(
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call float @llvm.sqrt.f32(
; SHADERTEST: = call float @llvm.sqrt.f32(
; SHADERTEST: = call float @llvm.sqrt.f32(
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
