
#version 450 core

layout(location = 0) in vec4 colorIn1;
layout(location = 1) in vec4 colorIn2;
layout(location = 0) out vec4 color;
void main()
{
    bvec4 x = lessThan (ivec4(colorIn1), ivec4(colorIn2));
    color = vec4(x);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: icmp slt <4 x i32>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
