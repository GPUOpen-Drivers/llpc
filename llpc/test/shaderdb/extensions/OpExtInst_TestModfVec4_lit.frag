#version 450 core


layout(location = 0) in vec4 a0;
layout(location = 10) in vec4 b0;
layout(location = 0) out vec4 color;

void main()
{
    vec4 x = vec4(0);
    vec4 y = modf(a0, x);
    color = vec4(x + y);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.trunc.v4f32(<4 x float>
; SHADERTEST: = fsub reassoc nnan nsz arcp contract afn <4 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
