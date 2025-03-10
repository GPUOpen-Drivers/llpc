#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) in vec4 colorIn1;
layout(location = 0) out vec4 color;
void main()
{
    uvec4 bd = uvec4(colorIn1);
    uint outi = 0;
    uint out1 = uaddCarry(bd.x,bd.y,outi);
    color = vec4(out1 + outi);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32
; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: call { i32, i1 } @llvm.uadd.with.overflow.i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
