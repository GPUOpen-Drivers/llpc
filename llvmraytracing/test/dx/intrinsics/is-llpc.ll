; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 3
; RUN: opt --verify-each -passes='dxil-cont-prepare-gpurt-library,lint' -S %s --lint-abort-on-error | FileCheck %s

declare i1 @_AmdIsLlpc()

%struct.DispatchSystemData = type { i32 }
declare !pointeetys !8 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*)

@debug_global = external global i32

define void @main() !lgc.rt.shaderstage !1 {
; CHECK-LABEL: define void @main(
; CHECK-SAME: ) !lgc.rt.shaderstage [[META1:![0-9]+]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    store i1 false, ptr @debug_global, align 1
; CHECK-NEXT:    ret void
;
entry:
  %val = call i1 @_AmdIsLlpc()
  store i1 %val, ptr @debug_global
  ret void
}

!0 = !{i32 2}
!1 = !{i32 0}
!8 = !{%struct.DispatchSystemData poison}
