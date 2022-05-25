; Check that an error is produced when the --mcpu flag is used with amdllpc.

; BEGIN_SHADERTEST
; RUN: not amdllpc --mcpu=10.3.0 %s | FileCheck --check-prefix=SHADERTEST %s
;
; SHADERTEST-LABEL: {{^}}ERROR: Option --mcpu is not supported in amdllpc, use --gfxip instead!
; SHADERTEST-LABEL: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST

define i32 @foo(i32 %x) {
  ret i32 %x
}
