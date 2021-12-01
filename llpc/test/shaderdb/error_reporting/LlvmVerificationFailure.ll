; Check that an error is produced when parsable but invalid LLVM IR is passed.

; BEGIN_SHADERTEST
; RUN: not amdllpc -v %gfxip %s | FileCheck --check-prefix=SHADERTEST %s
;
; SHADERTEST-LABEL: {{^}}ERROR: Result::ErrorInvalidShader: File {{.*}} parsed, but failed to verify the module:
; SHADERTEST-SAME:  Instruction does not dominate all uses!
; SHADERTEST-LABEL: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST

define i32 @f1(i32 %x) {
  %y = add i32 %z, 1
  %z = add i32 %x, 1
  ret i32 %y
}
