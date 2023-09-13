#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;
    dvec3 d3_1, d3_2, d3_3;
    dvec4 d4_1, d4_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    dvec3 d3_0 = mix(d3_1, d3_2, d3_3);

    dvec4 d4_0 = mix(d4_1, d4_2, d1_1);

    fragColor = (d3_0.y == d4_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fsub reassoc nnan nsz arcp contract <3 x double>
; SHADERTEST: = fmul reassoc nnan nsz arcp contract <3 x double>
; SHADERTEST: = fadd reassoc nnan nsz arcp contract <3 x double>
; SHADERTEST: = fsub reassoc nnan nsz arcp contract <4 x double>
; SHADERTEST: = fmul reassoc nnan nsz arcp contract <4 x double>
; SHADERTEST: = fadd reassoc nnan nsz arcp contract <4 x double>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
