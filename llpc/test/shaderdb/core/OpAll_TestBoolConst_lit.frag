#version 450 core

layout(location = 0) in vec4 colorIn1;
layout(location = 0) out vec4 color;
void main()
{

    bvec4 bd = bvec4(colorIn1);
    bvec4 bc = bvec4(true, false, true, false);
    color = vec4(all(bd) || all(bc));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: store <4 x i32> <i32 1, i32 0, i32 1, i32 0>,
; SHADERTEST-COUNT-6: = and i1
; SHADERTEST: = or i1

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
