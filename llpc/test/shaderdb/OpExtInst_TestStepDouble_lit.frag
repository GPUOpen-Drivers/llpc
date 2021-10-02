#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;

    dvec3 d3_1, d3_2;
    dvec4 d4_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    dvec3 d3_0 = step(d3_1, d3_2);

    dvec4 d4_0 = step(d1_1, d4_1);

    fragColor = (d3_0.y == d4_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract olt <3 x double>
; SHADERTEST: = select reassoc nnan nsz arcp contract <3 x i1> %{{[^, ]+}}, <3 x double> zeroinitializer, <3 x double> <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract olt <4 x double>
; SHADERTEST: = select reassoc nnan nsz arcp contract <4 x i1> %{{[^, ]+}}, <4 x double> zeroinitializer, <4 x double> <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
