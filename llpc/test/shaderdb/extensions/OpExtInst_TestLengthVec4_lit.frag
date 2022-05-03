#version 450 core

layout(location = 0) in vec4 a0;
layout(location = 10) in vec4 b0;
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(length(a0));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.dot.product.f32(<4 x float>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.sqrt.f32(float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
