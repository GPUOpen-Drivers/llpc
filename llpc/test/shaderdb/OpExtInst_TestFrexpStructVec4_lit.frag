#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;
layout(location = 0) out vec4 frag_color;

void main()
{
    // FrexpStruct
    ivec4 fo = ivec4(0);
    vec4 fv = frexp(a, fo);
    frag_color = vec4(fv * fo);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x float> (...) @lgc.create.extract.significand.v4f32(<4 x float>
; SHADERTEST: = call <4 x i32> (...) @lgc.create.extract.exponent.v4i32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST-DAG: = call reassoc nnan nsz arcp contract float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST-DAG: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST-DAG: = call reassoc nnan nsz arcp contract float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST-DAG: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST-DAG: = call reassoc nnan nsz arcp contract float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST-DAG: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST-DAG: = call reassoc nnan nsz arcp contract float @llvm.amdgcn.frexp.mant.f32(float
; SHADERTEST-DAG: = call i32 @llvm.amdgcn.frexp.exp.i32.f32(float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
