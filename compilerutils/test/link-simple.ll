; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --tool cross-module-inline --version 3
; RUN: cross-module-inline %s %S/inc/link-simple.ll --link inline_fun | FileCheck %s
;
; Simply inline a function

declare i32 @inline_fun(i32)

define i32 @main() {
; CHECK-LABEL: define i32 @main() {
; CHECK-NEXT:    ret i32 5
;
  %result = call i32 @inline_fun(i32 4)
  ret i32 %result
}
