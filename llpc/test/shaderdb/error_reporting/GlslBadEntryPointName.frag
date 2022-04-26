/*
; BEGIN_SHADERTEST
; RUN: not amdllpc %gfxip %s,mainFs \
; RUN:   | FileCheck -check-prefix=SHADERTEST %s
;
; SHADERTEST-LABEL: {{^}}ERROR: Result::ErrorInvalidShader: GLSL requires the entry point to be 'main':
; SHADERTEST-LABEL: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST
*/

#version 460

layout(location = 0) out vec4 outColor;

void mainFs() {
  outColor = vec4(1.0, 1.0, 1.0, 1.0);
}
