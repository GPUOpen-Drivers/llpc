; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 3
; RUN: opt --verify-each -passes='dxil-cont-prepare-gpurt-library,lint,lower-raytracing-pipeline,lint' -S %s --lint-abort-on-error | FileCheck --check-prefix=LOWERRAYTRACINGPIPELINE %s
; RUN: opt --verify-each -passes='dxil-cont-prepare-gpurt-library,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,cleanup-continuations,lint' -S %s --lint-abort-on-error | FileCheck --check-prefix=CLEANUP %s

%struct.DispatchSystemData = type { i32 }
%struct.TraversalData = type { i32 }

@debug_global = external global i32
declare i32 @Val(i32)
declare void @_AmdComplete()
declare !pointeetys !2 <3 x i32> @_cont_DispatchRaysIndex3(%struct.DispatchSystemData*)
declare !pointeetys !2 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*)
declare !pointeetys !3 i1 @_cont_ReportHit(%struct.TraversalData* %data, float %t, i32 %hitKind)

define void @_cont_Traversal(%struct.TraversalData %data) #1 !lgc.rt.shaderstage !0 {
; LOWERRAYTRACINGPIPELINE-LABEL: define void @_cont_Traversal(
; LOWERRAYTRACINGPIPELINE-SAME: i32 [[RETURNADDR:%.*]], [[STRUCT_TRAVERSALDATA:%.*]] [[TMP0:%.*]], [8 x i32] [[PADDING:%.*]], [30 x i32] [[PAYLOAD:%.*]]) #[[ATTR0:[0-9]+]] !lgc.rt.shaderstage [[META4:![0-9]+]] !continuation [[META5:![0-9]+]] {
; LOWERRAYTRACINGPIPELINE-NEXT:  AllocaSpillBB:
; LOWERRAYTRACINGPIPELINE-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_TRAVERSALDATA]], align 8
; LOWERRAYTRACINGPIPELINE-NEXT:    [[PAYLOAD_SERIALIZATION_ALLOCA:%.*]] = alloca [30 x i32], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store [30 x i32] [[PAYLOAD]], ptr [[PAYLOAD_SERIALIZATION_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store [[STRUCT_TRAVERSALDATA]] [[TMP0]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[VAL:%.*]] = call i32 @Val(i32 5)
; LOWERRAYTRACINGPIPELINE-NEXT:    call void @lgc.cps.complete()
; LOWERRAYTRACINGPIPELINE-NEXT:    store i32 [[VAL]], ptr @debug_global, align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    unreachable
;
; CLEANUP-LABEL: define void @_cont_Traversal(
; CLEANUP-SAME: i32 [[CSPINIT:%.*]], i32 [[RETURNADDR:%.*]], [[STRUCT_TRAVERSALDATA:%.*]] [[TMP0:%.*]], [8 x i32] [[PADDING:%.*]], [30 x i32] [[PAYLOAD:%.*]]) #[[ATTR0:[0-9]+]] !lgc.rt.shaderstage [[META4:![0-9]+]] !continuation [[META5:![0-9]+]] !continuation.state [[META1:![0-9]+]] {
; CLEANUP-NEXT:  AllocaSpillBB:
; CLEANUP-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CLEANUP-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CLEANUP-NEXT:    [[PAYLOAD_FCA_0_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 0
; CLEANUP-NEXT:    [[PAYLOAD_FCA_1_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 1
; CLEANUP-NEXT:    [[PAYLOAD_FCA_2_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 2
; CLEANUP-NEXT:    [[PAYLOAD_FCA_3_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 3
; CLEANUP-NEXT:    [[PAYLOAD_FCA_4_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 4
; CLEANUP-NEXT:    [[PAYLOAD_FCA_5_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 5
; CLEANUP-NEXT:    [[PAYLOAD_FCA_6_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 6
; CLEANUP-NEXT:    [[PAYLOAD_FCA_7_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 7
; CLEANUP-NEXT:    [[PAYLOAD_FCA_8_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 8
; CLEANUP-NEXT:    [[PAYLOAD_FCA_9_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 9
; CLEANUP-NEXT:    [[PAYLOAD_FCA_10_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 10
; CLEANUP-NEXT:    [[PAYLOAD_FCA_11_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 11
; CLEANUP-NEXT:    [[PAYLOAD_FCA_12_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 12
; CLEANUP-NEXT:    [[PAYLOAD_FCA_13_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 13
; CLEANUP-NEXT:    [[PAYLOAD_FCA_14_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 14
; CLEANUP-NEXT:    [[PAYLOAD_FCA_15_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 15
; CLEANUP-NEXT:    [[PAYLOAD_FCA_16_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 16
; CLEANUP-NEXT:    [[PAYLOAD_FCA_17_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 17
; CLEANUP-NEXT:    [[PAYLOAD_FCA_18_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 18
; CLEANUP-NEXT:    [[PAYLOAD_FCA_19_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 19
; CLEANUP-NEXT:    [[PAYLOAD_FCA_20_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 20
; CLEANUP-NEXT:    [[PAYLOAD_FCA_21_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 21
; CLEANUP-NEXT:    [[PAYLOAD_FCA_22_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 22
; CLEANUP-NEXT:    [[PAYLOAD_FCA_23_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 23
; CLEANUP-NEXT:    [[PAYLOAD_FCA_24_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 24
; CLEANUP-NEXT:    [[PAYLOAD_FCA_25_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 25
; CLEANUP-NEXT:    [[PAYLOAD_FCA_26_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 26
; CLEANUP-NEXT:    [[PAYLOAD_FCA_27_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 27
; CLEANUP-NEXT:    [[PAYLOAD_FCA_28_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 28
; CLEANUP-NEXT:    [[PAYLOAD_FCA_29_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 29
; CLEANUP-NEXT:    [[DOTFCA_0_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0
; CLEANUP-NEXT:    [[VAL:%.*]] = call i32 @Val(i32 5)
; CLEANUP-NEXT:    ret void
;
AllocaSpillBB:
  %val = call i32 @Val(i32 5)
  call void @_AmdComplete()
  store i32 %val, i32* @debug_global, align 4
  unreachable
}

!0 = !{i32 6}
!2 = !{%struct.DispatchSystemData poison}
!3 = !{%struct.TraversalData poison}
