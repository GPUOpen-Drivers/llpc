; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 3
; RUN: opt --verify-each -passes="dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint" -S %s --lint-abort-on-error | FileCheck %s

%struct.DispatchSystemData = type { i32 }
%struct.Payload = type { i32 }

@debug_global = external global i32

declare i32 @lgc.rt.shader.index()

declare !pointeetys !8 <3 x i32> @_cont_DispatchRaysIndex3(%struct.DispatchSystemData*)

declare !pointeetys !8 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*)

define i1 @_cont_ReportHit(%struct.DispatchSystemData* %data, float %t, i32 %hitKind) #0 !pointeetys !20 {
  ret i1 true
}

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !{%struct.DispatchSystemData poison} {
  ret void
}

define void @main() !lgc.rt.shaderstage !24 {
; CHECK-LABEL: define void @main(
; CHECK-SAME: i32 [[RETURNADDR:%.*]], i32 [[SHADER_INDEX:%.*]], [[STRUCT_DISPATCHSYSTEMDATA:%.*]] [[TMP0:%.*]]) !lgc.rt.shaderstage [[META12:![0-9]+]] !lgc.cps [[META10:![0-9]+]] !continuation.registercount [[META12]] !continuation [[META13:![0-9]+]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; CHECK-NEXT:    [[PAYLOAD_SERIALIZATION_ALLOCA:%.*]] = alloca [0 x i32], align 4
; CHECK-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP0]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; CHECK-NEXT:    call void @amd.dx.setLocalRootIndex(i32 0)
; CHECK-NEXT:    store i32 0, ptr @debug_global, align 4
; CHECK-NEXT:    call void @lgc.cps.complete()
; CHECK-NEXT:    unreachable
;
entry:
  %val = call i32 @lgc.rt.shader.index()
  store i32 %val, ptr @debug_global
  ret void
}

define void @callable(%struct.Payload* %payload) !pointeetys !22 !lgc.rt.shaderstage !25 {
; CHECK-LABEL: define void @callable(
; CHECK-SAME: i32 [[RETURNADDR:%.*]], i32 [[SHADER_INDEX:%.*]], [[STRUCT_DISPATCHSYSTEMDATA:%.*]] [[SYSTEM_DATA:%.*]], {} [[HIT_ATTRS:%.*]], [8 x i32] [[PADDING:%.*]], [1 x i32] [[PAYLOAD:%.*]]) !lgc.rt.shaderstage [[META14:![0-9]+]] !lgc.cps [[META15:![0-9]+]] !continuation.registercount [[META10]] !continuation [[META16:![0-9]+]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; CHECK-NEXT:    [[PAYLOAD_SERIALIZATION_ALLOCA:%.*]] = alloca [1 x i32], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = alloca [[STRUCT_PAYLOAD:%.*]], align 8
; CHECK-NEXT:    store [1 x i32] [[PAYLOAD]], ptr [[PAYLOAD_SERIALIZATION_ALLOCA]], align 4
; CHECK-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[SYSTEM_DATA]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = getelementptr inbounds [[STRUCT_PAYLOAD]], ptr [[TMP0]], i32 0
; CHECK-NEXT:    [[TMP2:%.*]] = load i32, ptr [[PAYLOAD_SERIALIZATION_ALLOCA]], align 4
; CHECK-NEXT:    store i32 [[TMP2]], ptr [[TMP1]], align 4
; CHECK-NEXT:    call void @amd.dx.setLocalRootIndex(i32 [[SHADER_INDEX]])
; CHECK-NEXT:    store i32 [[SHADER_INDEX]], ptr @debug_global, align 4
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr inbounds [[STRUCT_PAYLOAD]], ptr [[TMP0]], i32 0
; CHECK-NEXT:    [[TMP4:%.*]] = load i32, ptr [[TMP3]], align 4
; CHECK-NEXT:    store i32 [[TMP4]], ptr [[PAYLOAD_SERIALIZATION_ALLOCA]], align 4
; CHECK-NEXT:    [[TMP5:%.*]] = load [[STRUCT_DISPATCHSYSTEMDATA]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; CHECK-NEXT:    [[TMP6:%.*]] = load [1 x i32], ptr [[PAYLOAD_SERIALIZATION_ALLOCA]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[RETURNADDR]], i32 6, i32 poison, i32 poison, i32 poison, [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP5]], [8 x i32] poison, [1 x i32] [[TMP6]]), !continuation.registercount [[META10]]
; CHECK-NEXT:    unreachable
;
entry:
  %val = call i32 @lgc.rt.shader.index()
  store i32 %val, ptr @debug_global
  ret void
}

!dx.entryPoints = !{!0, !3, !10}
!continuation.stackAddrspace = !{!7}
!lgc.cps.module = !{}

!0 = !{null, !"", null, !1, !6}
!1 = !{!2, null, null, null}
!2 = !{!3}
!3 = !{i1 ()* @main, !"main", null, null, !4}
!4 = !{i32 8, i32 7}
!6 = !{i32 0, i64 65536}
!7 = !{i32 21}
!8 = !{%struct.DispatchSystemData poison}
!9 = !{i32 0, %struct.DispatchSystemData poison}
!10 = !{i1 ()* @callable, !"callable", null, null, !11}
!11 = !{i32 8, i32 12}
!20 = !{%struct.DispatchSystemData poison}
!21 = !{i32 0, %struct.DispatchSystemData poison}
!22 = !{%struct.Payload poison}
!23 = !{i32 0, %struct.Payload poison}
!24 = !{i32 0}
!25 = !{i32 5}
