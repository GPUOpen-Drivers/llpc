#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(binding = 0) uniform Uniforms
{
    mat2x3 m2x3;
    dmat2x3 dm2x3_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    dmat2x3 dm2x3 = m2x3;

    fragColor = (dm2x3[0] != dm2x3_2[1]) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST-COUNT-1: fpext <3 x float> {{.*}} to <3 x double>
; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST-COUNT-3: fpext float {{.*}} to double
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
