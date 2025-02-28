#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(binding = 0, std430) buffer Buffers
{
    float f1;
    vec4  f4;
};

void main()
{
    f1 *= 2;
    memoryBarrierBuffer();
    f4 = vec4(f1);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: fence syncscope("agent") acq_rel

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
