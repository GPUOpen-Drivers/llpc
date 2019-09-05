#version 450 core

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
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call float (...) @llpc.call.power.f32(float 2.000000e+00, float
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: = call float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST: = call float @ldexpf(float 1.000000e+00, i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
