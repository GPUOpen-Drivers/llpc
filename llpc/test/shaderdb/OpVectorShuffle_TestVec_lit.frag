#version 450

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform Uniforms
{
    vec3 color;
};

void main()
{
    vec4 data  = vec4(0.5);

    data.xw = color.zy;

    fragColor = data;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{.*}} = extractelement <2 x float> %{{.*}}, i32 0
; SHADERTEST: %{{.*}} = insertelement <4 x float> {{undef|poison}}, float %{{.*}}, i32 0
; SHADERTEST: %{{.*}} = extractelement <2 x float> %{{.*}}, i32 1
; SHADERTEST: %{{.*}} = insertelement <4 x float> %{{.*}}, float %{{.*}}, i32 3
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: [[VEC1:%.*]] = shufflevector <3 x float> %{{.*}}, <3 x float> {{undef|poison}}, <4 x i32> <i32 {{undef|poison}}, i32 1, i32 2, i32 {{undef|poison}}>
; SHADERTEST: [[VEC2:%.*]] = shufflevector <4 x float> <float {{undef|poison}}, float 5.000000e-01, float 5.000000e-01, float {{undef|poison}}>, <4 x float> [[VEC1]], <4 x i32> <i32 6, i32 1, i32 2, i32 5>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
