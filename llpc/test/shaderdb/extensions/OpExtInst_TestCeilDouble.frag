#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(binding = 0) uniform Uniforms
{
    double d1_1;
    dvec3 d3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    double d1_0 = ceil(d1_1);

    dvec3 d3_0 = ceil(d3_1);

    fragColor = ((d1_0 != d3_0.x)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
