#version 450

layout(binding = 0) uniform Uniforms
{
    uint u1_1, u1_2;
    uvec3 u3_1, u3_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    uint u1_0, u1_3;
    u1_0 = uaddCarry(u1_1, u1_2, u1_3);

    uvec3 u3_0, u3_3;
    u3_0 = uaddCarry(u3_1, u3_2, u3_3);

    fragColor = ((u1_0 != u1_3) || (u3_0 == u3_3)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32
; SHADERTEST: = call { <3 x i32>, <3 x i1> } @llvm.uadd.with.overflow.v3i32(<3 x i32>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
