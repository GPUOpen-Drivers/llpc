// Check that an invalid gfxip generates the appropriate error
/*
; RUN: not amdllpc -v -gfxip=1.2.3 %s | FileCheck -check-prefix=SHADERTEST %s
;
; SHADERTEST-LABEL: {{^}}ERROR: Invalid gfxip: gfx123
; SHADERTEST-LABEL: {{^}}===== AMDLLPC FAILED =====
*/

#version 460

layout (location = 0) out vec4 fragColor;

void main() {
  fragColor = vec4(0);
}

