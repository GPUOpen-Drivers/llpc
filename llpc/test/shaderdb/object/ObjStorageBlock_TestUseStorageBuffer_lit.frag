#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


#pragma use_storage_buffer

layout(std430, binding = 0) buffer Buffers
{
    vec4  f4;
    float f1[];
};

layout(location = 0) out vec4 f;

void main()
{
    f = f4 + vec4(f1.length());
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call i64 {{.*}}@lgc.buffer.length(

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
