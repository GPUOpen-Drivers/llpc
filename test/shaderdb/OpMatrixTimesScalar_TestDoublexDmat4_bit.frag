#version 450

layout(binding = 0) uniform Uniforms
{
    double d1;
    dmat4  dm4;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    dmat4 dm4_0 = d1 * dm4;
    fragColor = vec4(dm4_0[1]);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [4 x <4 x double>] (...) @llpc.call.matrix.times.scalar.a4v4f64

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: fmul double %{{[^, ]*}}, %{{[^, ]*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
