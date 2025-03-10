#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) in float fIn;
layout(location = 0) out float fOut;

void main()
{
    int exp = 0;
    float f = frexp(fIn, exp);
    f += pow(2.0, exp);
    fOut = f;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.power.f32(float 2.000000e+00, float
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: = call float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.{{(ldexp.f32.i32\(float 1.000000e\+00, i32)|(exp2.f32\(float)}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
