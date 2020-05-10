#version 450 core

layout(location = 0) in vec4 a0;
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(length(a0.x) + length(double(a0.x)));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.fabs.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract double @llvm.fabs.f64(double
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
