#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;
    dvec3 d3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    double d1_0 = sqrt(d1_1);

    dvec3 d3_0 = sqrt(d3_1);

    fragColor = (d1_0 >= d3_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn double @llvm.sqrt.f64(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x double> @llvm.sqrt.v3f64(
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn double @llvm.sqrt.f64(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn double @llvm.sqrt.f64(
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
