#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(binding = 0) uniform Uniforms
{
    ivec2 i2_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    ivec2 i2_0 = ivec2(0);
    i2_0 %= i2_1;

    fragColor = (i2_0.y != 4) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call <2 x i32> (...) @lgc.create.smod.v2i32(<2 x i32>

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
