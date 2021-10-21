; Check that an error is produced when parsable but invalid LLVM IR is passed.

; BEGIN_SHADERTEST
; RUN: not amdllpc -v %gfxip %s | FileCheck --check-prefix=SHADERTEST %s
;
; SHADERTEST-LABEL: {{^}}ERROR: File {{.*}} parsed, but fail to verify the module: Instruction does not dominate all uses!
; SHADERTEST-LABEL: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST

define i32 @f1(i32 %x) {
  %y = add i32 %z, 1
  %z = add i32 %x, 1
  ret i32 %y
}
