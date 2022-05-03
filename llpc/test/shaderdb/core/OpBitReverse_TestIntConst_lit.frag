#version 450 core

layout(location = 0) out vec4 color;
void main()
{
    color = vec4(bitfieldReverse(342));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call i32 @llvm.bitreverse.i32(i32 342)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
