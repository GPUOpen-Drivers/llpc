// Check that an error is produced when the same shader stage is provided twice.
/*
; BEGIN_SHADERTEST
; RUN: not amdllpc -v %gfxip -spvgen-dir=%spvgendir% %s %s \
; RUN:   | FileCheck -check-prefix=SHADERTEST %s
;
; SHADERTEST-LABEL: {{^}}ERROR: Result::ErrorInvalidShader: Duplicate shader stage (fragment)
; SHADERTEST-LABEL: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST
*/

#version 460

layout (location = 0) out vec4 fragColor;

void main() {
  fragColor = vec4(0);
}
