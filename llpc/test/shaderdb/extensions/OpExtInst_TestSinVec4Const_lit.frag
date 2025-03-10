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
    vec4 fv = sin(a);
    fv.x = sin(1.5);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.sin.v4f32(
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.sin.v4f32(
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.sin.f32(float %{{.*}})
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.sin.f32(float %{{.*}})
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.sin.f32(float %{{.*}})
; SHADERTEST-NOT: = call{{.*}} float @llvm.sin.f32(float %{{.*}})
; SHADERTEST: ret
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
