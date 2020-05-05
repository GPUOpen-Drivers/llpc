#version 450

layout(binding = 0) uniform Uniforms
{
    mat2 m2_1;
    dmat4 dm4_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    mat2 m2_0;
    m2_0 = m2_1 + m2_1;

    dmat4 dm4_0;
    dm4_0 = dm4_1 + dm4_1;

    fragColor = ((m2_0[0] != m2_0[1]) || (dm4_0[2] != dm4_0[3])) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST-COUNT-2: fadd reassoc nnan nsz arcp contract afn <2 x float>
; SHADERTEST-COUNT-2: fadd reassoc nnan nsz arcp contract afn <4 x double>
; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
