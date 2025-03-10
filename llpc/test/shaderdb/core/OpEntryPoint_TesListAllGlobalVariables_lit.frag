#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) in vec4 inv;
layout(location = 0) out vec4 outv;

vec4 globalv;

layout(binding = 0) uniform ubt {
    vec4 v;
} uniformv;

layout(binding = 1) buffer bbt {
    float f;
} bufferv;

layout(push_constant) uniform pushB {
    int a;
} pushv;

void main()
{
    vec4 functionv;
    functionv = inv;
    globalv = inv;
    outv = functionv + inv + globalv + uniformv.v * pushv.a * bufferv.f;
    outv += functionv + inv + globalv + uniformv.v * pushv.a * bufferv.f;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
