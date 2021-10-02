#version 450

layout(binding = 0) uniform Uniforms
{
    dmat4x3 dm4x3;
    dmat3x4 dm3x4;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    dmat3 dm3 = dm4x3 * dm3x4;

    fragColor = (dm3[0] == dm3[1]) ? vec4(1.0) : color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [3 x <3 x double>] (...) @lgc.create.matrix.times.matrix.a3v3f64

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: fmul {{.*}}double %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: fadd {{.*}}double %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
