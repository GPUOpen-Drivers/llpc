#version 450

layout(binding = 0) uniform Uniforms
{
    uint u1_1, u1_2;
    uvec3 u3_1, u3_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    uint u1_0 = max(u1_1, u1_2);

    uvec3 u3_0 = max(u3_1, u3_2);

    fragColor = (u1_0 != u3_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call i32 @llvm.umax.i32(i32 %{{[^, ]+}}, i32 %{{[^) ]+}})
; SHADERTEST: = call <3 x i32> @llvm.umax.v3i32(<3 x i32> %{{[^, ]+}}, <3 x i32> %{{[^) ]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
