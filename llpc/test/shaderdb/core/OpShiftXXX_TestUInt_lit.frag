#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) in vec4 a0;
layout(location = 1) in vec4 b0;
layout(location = 0) out vec4 color;
void main()
{
    uvec4 ia = uvec4(a0);
    uint x = ((ia.x >> ia.y) & ia.z | (ia.w)) << (ia.x);
    uvec4 ca = ivec4(10, 4, -3, -9);
    uint y = ((ca.x >> ca.y) & ca.z | (ca.w)) << (ca.x);
    color = vec4(x + y);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: lshr
; SHADERTEST: shl

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
