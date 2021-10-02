#version 450 core

layout(location = 0) in vec2 l0 ;
layout(location = 1) in vec2 l1 ;
layout(location = 2) in vec2 l2 ;
layout(location = 0) out vec4 color;


void main()
{
    mat2 x = outerProduct(l0,l1);
    mat2 y = outerProduct(l0,l2);
    mat2 z = x * y;
    color.xy = z[0];
    color.wz = z[1];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [2 x <2 x float>] (...) @lgc.create.matrix.times.matrix.a2v2f32

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: fmul {{.*}}float %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: fadd {{.*}}float %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
