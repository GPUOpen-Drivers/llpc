#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(binding = 0) uniform Uniforms
{
    ivec3 i3_1, i3_2;
    uvec2 u2_1, u2_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    ivec3 i3_0 = i3_1 + i3_2;

    uvec2 u2_0 = u2_1 + u2_2;

    fragColor = ((i3_0.y != 5) && (u2_0.x != 7)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: add <3 x i32>
; SHADERTEST: add <2 x i32>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
