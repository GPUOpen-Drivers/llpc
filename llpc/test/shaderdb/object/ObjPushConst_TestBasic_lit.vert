#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(push_constant) uniform PCB
{
    vec4 m1;
} g_pc;

void main()
{
    gl_Position = g_pc.m1;
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} FE lowering results
; SHADERTEST:  [[V0:%.*]] = call {{.*}} @lgc.create.load.push.constants.ptr
; SHADERTEST:  load <4 x float>, ptr addrspace(4) [[V0]], align 16

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
