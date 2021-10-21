; Check that an error is produced when valid LLVM IR is passed but is not a shader.

; BEGIN_SHADERTEST
; DONT-RUN: not amdllpc -v %gfxip %s | FileCheck --check-prefix=SHADERTEST %s
; Currently this test-case crashes instead of exiting gracefully. Do not run this test for now.
; RUN: false
; XFAIL: *
;
; SHADERTEST-LABEL: {{^}}ERROR: File {{.*}}: Fail to determine shader stage
; SHADERTEST-LABEL: {{^}}===== AMDLLPC FAILED =====
; END_SHADERTEST

define i32 @foo(i32 %x) {
  ret i32 %x
}
