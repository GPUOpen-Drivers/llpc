#version 450

layout(binding = 0) uniform Uniforms
{
    dvec4 d4;
    dmat4x2 dm4x2;
    dvec2 d2_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    dvec2 d2_0 = dm4x2 * d4;

    fragColor = (d2_0 == d2_1) ? vec4(1.0) : color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: <2 x double> (...) @lgc.create.matrix.times.vector.v2f64

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: fmul reassoc nnan nsz arcp contract double
; SHADERTEST: fadd reassoc nnan nsz arcp contract double

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
