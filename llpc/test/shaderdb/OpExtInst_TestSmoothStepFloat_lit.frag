#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1, f1_2;

    vec3 f3_1, f3_2, f3_3;
    vec4 f4_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 f3_0 = smoothstep(f3_1, f3_2, f3_3);

    vec4 f4_0 = smoothstep(f1_1, f1_2, f4_1);

    fragColor = (f3_0.y == f4_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x float> (...) @lgc.create.smooth.step.v3f32(<3 x float>
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x float> (...) @lgc.create.smooth.step.v4f32(<4 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
