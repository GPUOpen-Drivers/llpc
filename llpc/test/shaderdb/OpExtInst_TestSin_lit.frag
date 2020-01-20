#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    vec3 f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = sin(f1_1);

    vec3 f3_0 = sin(f3_1);

    fragColor = (f1_0 != f3_0.x) ? vec4(0.5) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.sin.f32(
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x float> @llvm.sin.v3f32(
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.sin.f32(
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x float> @llvm.sin.v3f32(
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
