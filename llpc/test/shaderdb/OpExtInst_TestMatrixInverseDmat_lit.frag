#version 450

layout(binding = 0) uniform Uniforms
{
    dmat2 dm2_1;
    dmat3 dm3_1;
    dmat4 dm4_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    dmat2 dm2_0 = inverse(dm2_1);

    dmat3 dm3_0 = inverse(dm3_1);

    dmat4 dm4_0 = inverse(dm4_1);

    fragColor = ((dm2_0[0] != dm2_0[1]) || (dm3_0[0] != dm3_0[1]) || (dm4_0[0] != dm4_0[1])) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call {{.*}}[2 x <2 x double>] (...) @lgc.create.matrix.inverse.a2v2f64([2 x <2 x double>] %
; SHADERTEST: = call {{.*}}[3 x <3 x double>] (...) @lgc.create.matrix.inverse.a3v3f64([3 x <3 x double>] %
; SHADERTEST: = call {{.*}}[4 x <4 x double>] (...) @lgc.create.matrix.inverse.a4v4f64([4 x <4 x double>] %
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
