; RUN: not --crash opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint' -S %s 2>&1 | FileCheck %s

; CHECK: ERROR: Did not find function '' requested by _AmdGetFuncAddr

%struct.DispatchSystemData = type { i32 }

declare i64 @_AmdGetFuncAddr()

declare %struct.DispatchSystemData @_cont_SetupRayGen()

declare !types !8 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*)

define i64 @main() {
entry:
  %val = call i64 @_AmdGetFuncAddr()
  ret i64 %val
}

!dx.entryPoints = !{!0, !3}
!continuation.stackAddrspace = !{!7}

!0 = !{null, !"", null, !1, !6}
!1 = !{!2, null, null, null}
!2 = !{!3}
!3 = !{i1 ()* @main, !"main", null, null, !4}
!4 = !{i32 8, i32 7, i32 6, i32 16, i32 7, i32 8, i32 5, !5}
!5 = !{i32 0}
!6 = !{i32 0, i64 65536}
!7 = !{i32 21}
!8 = !{!"function", i32 poison, !9}
!9 = !{i32 0, %struct.DispatchSystemData poison}
