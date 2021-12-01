// Check that an error/warning is reported when no SPVGEN dir is provided or a wrong dir is passed.
/*
; BEGIN_SHADERTEST_1
; RUN: not amdllpc -spvgen-dir=/spvgen/is/definitely/not/here -v %gfxip %s \
; RUN:   | FileCheck -check-prefix=SHADERTEST_1 %s
;
; SHADERTEST_1-LABEL: {{^}}ERROR: Failed to load SPVGEN from specified directory
; SHADERTEST_1-LABEL: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST_1

; BEGIN_SHADERTEST_2
; RUN: not amdllpc -v %gfxip %s \
; RUN:   | FileCheck -check-prefix=SHADERTEST_2 %s
;
; SHADERTEST_2-LABEL: {{^}}ERROR: Result::ErrorUnavailable: Failed to load SPVGEN -- cannot compile GLSL
; SHADERTEST_2: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST_2
*/

#version 460

layout (location = 0) out vec4 fragColor;

void main() {
  fragColor = vec4(0);
}
