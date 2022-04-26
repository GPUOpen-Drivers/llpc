// BEGIN_SHADERTEST
// This is not the main test, just to make sure that the shader is valid.
// The real check will be included in the used pipelines.
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec4 outColor;

void main() {
  outColor = vec4(1.0, 0.0, 0.0, 1.0);
}
