#version 450

layout(binding = 0) uniform Uniforms
{
    dvec2 d2;
    dmat2x3 dm2x3;
    dvec3 d3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    dvec3 d3_0 = dm2x3 * d2;

    fragColor = (d3_0 == d3_1) ? vec4(1.0) : color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: <3 x double> (...) @lgc.create.matrix.times.vector.v3f64

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: fmul reassoc nnan nsz arcp contract afn double
; SHADERTEST: fadd reassoc nnan nsz arcp contract afn double

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
