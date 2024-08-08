; RUN: not --crash opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint' -S %s --lint-abort-on-error 2>&1 | FileCheck %s

; CHECK: ERROR: Did not find function '' requested by _AmdGetFuncAddr

%struct.DispatchSystemData = type { i32 }
%struct.TraversalData = type { }

declare i64 @_AmdGetFuncAddr()

declare !pointeetys !8 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*)

declare !pointeetys !11 i1 @_cont_ReportHit(%struct.TraversalData* %data, float %t, i32 %hitKind)

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !8 {
  ret void
}

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
!8 = !{%struct.DispatchSystemData poison}
!9 = !{i32 0, %struct.DispatchSystemData poison}
!10 = !{i32 0, %struct.TraversalData poison}
!11 = !{%struct.TraversalData poison}
