#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;
    dvec3 d3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    double d1_0 = inversesqrt(d1_1);

    dvec3 d3_0 = inversesqrt(d3_1);

    fragColor = (d1_0 >= d3_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[SQRT:[^ ,]*]] = call reassoc nnan nsz arcp contract double (...) @lgc.create.sqrt.f64(double
; SHADERTEST: = fdiv reassoc nnan nsz arcp contract double 1.000000e+00, %[[SQRT]]
; SHADERTEST: %[[SQRT3:[^ ,]*]] = call reassoc nnan nsz arcp contract <3 x double> (...) @lgc.create.sqrt.v3f64(<3 x double>
; SHADERTEST: = fdiv reassoc nnan nsz arcp contract <3 x double> <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>, %[[SQRT3]]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
