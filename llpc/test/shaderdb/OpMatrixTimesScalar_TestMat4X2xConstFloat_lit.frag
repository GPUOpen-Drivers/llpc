#version 450

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    mat4x2 m4x2 = mat4x2(1.0);
    m4x2 *= 0.5;

    fragColor = vec4(m4x2[0] + m4x2[1]  +  m4x2[2]  + m4x2[3], 0, 0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [4 x <2 x float>] (...) @lgc.create.matrix.times.scalar.a4v2f32

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: ret { <4 x float> } { <4 x float> <float 1.000000e+00, float 5.000000e-01, float 0.000000e+00, float 0.000000e+00> }

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
