#version 450 core

layout(location = 0) in vec4 colorIn1;
layout(location = 1) in vec2 colorIn2;
layout(location = 2) in vec2 colorIn3;

layout(location = 0) out vec4 color;

struct AA
{
   mat4 bb;
};

layout(binding=2) uniform BB
{
  mat3x4 m2;
};

void main()
{
    color = m2 * colorIn1.xyz;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: <4 x float> (...) @lgc.create.matrix.times.vector.v4f32

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: fmul reassoc nnan nsz arcp contract afn float
; SHADERTEST: fadd reassoc nnan nsz arcp contract afn float

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
