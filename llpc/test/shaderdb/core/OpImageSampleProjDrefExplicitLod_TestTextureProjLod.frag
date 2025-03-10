#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(set = 0, binding = 0) uniform sampler1DShadow    samp1DShadow;
layout(set = 1, binding = 0) uniform sampler2DShadow    samp2DShadow[4];

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1 = textureProjLod(samp1DShadow, vec4(0.8), 1.0);
    f1 += textureProjLod(samp2DShadow[index], vec4(0.5), 2.0);

    fragColor = vec4(f1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
