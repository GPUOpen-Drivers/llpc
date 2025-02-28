#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(std430, set = 0, binding = 0) buffer Data
{
    int  i1;
    vec3 f3;
    bool b1[];
} data[4];

layout(std430, set = 0, binding = 1) buffer OtherData
{
    vec2 f2;
    mat4 m4[];
} otherData;

layout(location = 0) out vec4 f;

void main()
{
    int len = data[2].b1.length();
    len += otherData.m4.length();
    f = vec4(len);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call i64 {{.*}}@lgc.buffer.length(
; SHADERTEST: call i64 {{.*}}@lgc.buffer.length(

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
