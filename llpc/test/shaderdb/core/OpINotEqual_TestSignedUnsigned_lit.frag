#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(set = 0, binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 color;

void main()
{
    if (bool(i))
    {
        color = vec4(1, 1, 1, 1);
    }
    else
    {
        color = vec4(0, 0, 0, 0);
    }
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = icmp ne i32 %{{[0-9a-zA-Z.]*}}, 0

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: icmp eq i32 %{{[0-9a-zA-Z.]*}}, 0

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
