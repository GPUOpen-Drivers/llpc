#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;
    double d1_2;
    double d1_3;

    dvec4 d4_1;
    dvec4 d4_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    double d1_0 = refract(d1_1, d1_2, d1_3);

    dvec4 d4_0 = refract(d4_1, d4_2, d1_3);

    fragColor = (d4_0.x > d1_0) ? vec4(0.5) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract double (...) @lgc.create.refract.f64(double
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x double> (...) @lgc.create.refract.v4f64(<4 x double>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
