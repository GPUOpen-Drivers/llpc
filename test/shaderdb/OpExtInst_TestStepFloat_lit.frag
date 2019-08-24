#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;

    vec3 f3_1, f3_2;
    vec4 f4_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 f3_0 = step(f3_1, f3_2);

    vec4 f4_0 = step(f1_1, f4_1);

    fragColor = (f3_0.y == f4_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract olt <3 x float>
; SHADERTEST: = select reassoc nnan nsz arcp contract <3 x i1> %{{.*}}, <3 x float> zeroinitializer, <3 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract olt <4 x float>
; SHADERTEST: = select reassoc nnan nsz arcp contract <4 x i1> %{{.*}}, <4 x float> zeroinitializer, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
