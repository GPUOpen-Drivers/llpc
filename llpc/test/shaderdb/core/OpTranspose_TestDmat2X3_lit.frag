#version 450

layout(binding = 0) uniform Uniforms
{
    dmat2x3 dm2x3;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    dmat3x2 dm3x2 = transpose(dm2x3);

    fragColor = (dm3x2[0][0] > 0.0) ? vec4(1.0) : vec4(0.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [3 x <2 x double>] {{.*}}@lgc.create.transpose.matrix.a3v2f64

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
