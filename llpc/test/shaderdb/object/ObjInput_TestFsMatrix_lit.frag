#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 2) flat in mat4 m4;

layout(binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = m4[i];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-COUNT-4: call <4 x float> (...) @lgc.input.import.interpolated__v4f32{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST-COUNT-16: call float @llvm.amdgcn.interp.mov
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
