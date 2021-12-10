#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;
layout(location = 2) in vec4 c;

layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 f = fma(a, b, c);
    frag_color = f + fma(vec4(0.7), vec4(0.2), vec4(0.1));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.fmuladd.v4f32(<4 x float>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.fmuladd.v4f32(<4 x float> <float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000>, <4 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000>, <4 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
