; RUN: cross-module-inline %s %S/inc/link-func-metadata.ll --link inline_fun | FileCheck %s
;
; Check that metadata of referenced functions is preserved

; CHECK: declare !md !0 i32 @"main.cloned.{{.*}}"(i32)
; CHECK: !0 = !{!"abc"}

declare i32 @inline_fun(i32)

define i32 @main() {
  %result = call i32 @inline_fun(i32 4)
  ret i32 %result
}
