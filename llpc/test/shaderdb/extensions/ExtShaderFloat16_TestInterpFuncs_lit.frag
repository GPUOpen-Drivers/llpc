#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


#extension GL_AMD_gpu_shader_half_float: enable

layout(location = 0) in f16vec4 f16v4;

layout(location = 0) out vec2 fragColor;

void main()
{
    f16vec2 f16v2 = interpolateAtCentroid(f16v4).xy;
    f16v2 += interpolateAtSample(f16v4, 2).xy;
    f16v2 += interpolateAtOffset(f16v4, f16vec2(0.2hf)).xy;

    fragColor = f16v2;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST-COUNT-3:call reassoc nnan nsz arcp contract afn <4 x half> (...) @lgc.input.import.interpolated__v4f16(i1 false,
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
