// BEGIN_SHADERTEST
// This is not the main test, just to make sure that the shader is valid.
// The real check will be included in the used pipelines.
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450
#extension GL_ARB_separate_shader_objects : enable

void main() {
}
