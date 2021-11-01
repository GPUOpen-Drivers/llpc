; Check that an error is produced when valid LLVM IR is passed but is not a shader.

; BEGIN_SHADERTEST
; RUN: not amdllpc -v %gfxip %s | FileCheck --check-prefix=SHADERTEST %s
;
; SHADERTEST-LABEL: {{^}}ERROR: File {{.*}} parsed, but failed to determine shader stage
; SHADERTEST-LABEL: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST

define i32 @foo(i32 %x) {
  ret i32 %x
}
