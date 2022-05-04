#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    vec3 f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = sqrt(f1_1);

    vec3 f3_0 = sqrt(f3_1);

    fragColor = ((f1_0 != 0.0) && (f3_0.x < 0.1)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.sqrt.f32(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.sqrt.v3f32(
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
