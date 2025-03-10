#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 3) out mat4 m4[2];

layout(binding = 0) uniform Uniforms
{
    int i;
};

void main()
{
    m4[1][i] = vec4(1.0);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST-COUNT-9: call void @llvm.amdgcn.exp.f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
