#version 450 core

layout(location = 0) in vec2 l0 ;
layout(location = 1) in vec2 l1 ;
layout(location = 0) out vec4 color;


void main()
{
    mat2 x = outerProduct(l0,l1);
    color.xy = x[0];
    color.zw = x[1];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [2 x <2 x float>] (...) @lgc.create.outer.product.a2v2f32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
