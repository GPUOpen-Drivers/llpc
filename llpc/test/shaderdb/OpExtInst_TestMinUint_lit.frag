#version 450

layout(binding = 0) uniform Uniforms
{
    uint u1_1, u1_2;
    uvec3 u3_1, u3_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    uint u1_0 = min(u1_1, u1_2);

    uvec3 u3_0 = min(u3_1, u3_2);

    fragColor = (u1_0 != u3_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = icmp ult i32
; SHADERTEST: = select i1 %{{.*}}, i32
; SHADERTEST: = icmp ult <3 x i32>
; SHADERTEST: = select <3 x i1> %{{.*}}, <3 x i32>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
