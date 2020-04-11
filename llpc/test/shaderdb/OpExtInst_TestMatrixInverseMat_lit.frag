#version 450

layout(binding = 0) uniform Uniforms
{
    mat2 m2_1;
    mat3 m3_1;
    mat4 m4_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    mat2 m2_0 = inverse(m2_1);

    mat3 m3_0 = inverse(m3_1);

    mat4 m4_0 = inverse(m4_1);

    fragColor = ((m2_0[0] != m2_0[1]) || (m3_0[0] != m3_0[1]) || (m4_0[0] != m4_0[1])) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call {{.*}}[2 x <2 x float>] (...) @lgc.create.matrix.inverse.a2v2f32([2 x <2 x float>] %
; SHADERTEST: = call {{.*}}[3 x <3 x float>] (...) @lgc.create.matrix.inverse.a3v3f32([3 x <3 x float>] %
; SHADERTEST: = call {{.*}}[4 x <4 x float>] (...) @lgc.create.matrix.inverse.a4v4f32([4 x <4 x float>] %
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
