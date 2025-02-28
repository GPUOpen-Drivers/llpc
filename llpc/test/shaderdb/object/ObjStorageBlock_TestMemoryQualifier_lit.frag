#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(set = 1, binding = 0) coherent buffer Buffer
{
    readonly vec4 f4;
    restrict vec2 f2;
    volatile writeonly float f1;
} buf;

void main()
{
    vec4 texel = vec4(0.0);

    texel += buf.f4;
    texel.xy += buf.f2;
    buf.f1 = texel.w;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: %{{[0-9]*}} = call ptr addrspace(7) @lgc.load.buffer.desc(i64 1, i32 0, i32 0,
; SHADERTEST: %{{[0-9]*}} = load atomic float, ptr addrspace(7) %{{[0-9]*}} unordered, align 4
; SHADERTEST: store atomic float %{{[0-9a-z.]*}}, ptr addrspace(7) %{{[0-9]*}} unordered, align 4

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
