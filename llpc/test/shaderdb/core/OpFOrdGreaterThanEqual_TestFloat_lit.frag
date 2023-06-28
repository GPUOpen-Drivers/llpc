#version 450 core

layout(location = 0) in vec4 colorIn1;
layout(location = 1) in vec4 colorIn2;
layout(location = 0) out vec4 color;
void main()
{
    bvec4 x = greaterThanEqual (colorIn1, colorIn2);
    bvec4 y = greaterThanEqual (uvec4(colorIn1), uvec4(colorIn2));
    bvec4 z = greaterThanEqual (ivec4(colorIn1), ivec4(colorIn2));
    bvec4 w = equal(x,y);
    bvec4 q = notEqual(w,z);
    color = vec4(q);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: icmp uge <4 x i32>
; SHADERTEST: fcmp ult <4 x float>
; SHADERTEST: icmp sge <4 x i32>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
