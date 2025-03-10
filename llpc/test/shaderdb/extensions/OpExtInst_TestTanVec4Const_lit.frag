#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) in vec4 a;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = tan(a);
    fv.x = tan(1.5);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.tan.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.sin.v4f32(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.cos.v4f32(
; SHADERTEST: = fdiv reassoc nnan nsz arcp contract afn <4 x float> {{(splat \(float 1\.000000e\+00\))|(<float 1\.000000e\+00,)}}
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <4 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
