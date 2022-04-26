#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = log2(a);
    fv.x = log2(2.0);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.log2.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.log2.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.log2.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.log2.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.log2.f32(float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
