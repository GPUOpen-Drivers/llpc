; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: grep -v PRESERVED_REGCOUNT %s | opt --verify-each -passes='lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,dxil-cont-post-process,lint,continuations-lint,remove-types-metadata' -S --lint-abort-on-error | FileCheck --check-prefix=MAXPAYLOADSIZE %s
; RUN: opt --verify-each -passes='lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,dxil-cont-post-process,lint,continuations-lint,remove-types-metadata' -S %s --lint-abort-on-error | FileCheck --check-prefix=PRESERVEDPAYLOADSIZE %s

; Test that we pass either the maximum or the computed, preserved payload size through _cont_Traversal.

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.TraversalData = type { %struct.SystemData, i32 }
%struct.SystemData = type { %struct.DispatchSystemData, float }
%struct.DispatchSystemData = type { i32 }

!continuation.maxUsedPayloadRegisterCount = !{!8} ; PRESERVED_REGCOUNT

declare !pointeetys !4 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*)

declare !pointeetys !6 i1 @_cont_ReportHit(%struct.TraversalData* %data, float %t, i32 %hitKind)

declare void @lgc.cps.jump(...)

declare i64 @lgc.cps.as.continuation.reference__i64(...) #3

; Function Attrs: alwaysinline nounwind
define void @_cont_Traversal(%struct.TraversalData %data) #1 !lgc.rt.shaderstage !7 {
  %1 = alloca %struct.TraversalData, align 8
  store %struct.TraversalData %data, ptr %1, align 4
  %2 = getelementptr inbounds %struct.TraversalData, ptr %1, i32 0, i32 1
  %3 = load i32, ptr %2, align 4
  %4 = icmp eq i32 %3, 0
  %5 = getelementptr inbounds %struct.TraversalData, ptr %1, i32 0, i32 0
  br i1 %4, label %9, label %6

6:                                                ; preds = %0
  %7 = load %struct.SystemData, ptr %5, align 4
  %8 = call i64 (...) @lgc.cps.as.continuation.reference__i64(ptr @_cont_Traversal)
  call void (...) @lgc.cps.jump(i64 1, i32 -1, {} poison, i64 %8, %struct.SystemData %7), !waitmask !9
  unreachable

9:                                                ; preds = %0
  %10 = load %struct.SystemData, ptr %5, align 4
  call void (...) @lgc.cps.jump(i64 0, i32 -1, {} poison, i64 poison, %struct.SystemData %10), !waitmask !9
  unreachable
}

attributes #0 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { nounwind }

!0 = !{!"function", i32 poison, !1}
!1 = !{i32 0, %struct.TraversalData poison}
!2 = !{!"function", i32 poison, !1, i32 poison}
!3 = !{!"function", !"void", !1, i32 poison, i32 poison}
!4 = !{%struct.DispatchSystemData poison}
!5 = !{i32 0, %struct.DispatchSystemData poison}
!6 = !{%struct.TraversalData poison}
!7 = !{i32 6}
!8 = !{i32 4} ; PRESERVED_REGCOUNT
!9 = !{i32 -1}
; MAXPAYLOADSIZE-LABEL: define void @_cont_Traversal(
; MAXPAYLOADSIZE-SAME: i32 [[CSPINIT:%.*]], i64 [[RETURNADDR:%.*]], [[STRUCT_TRAVERSALDATA:%.*]] [[TMP0:%.*]], [8 x i32] [[PADDING:%.*]], [30 x i32] [[PAYLOAD:%.*]]) #[[ATTR0:[0-9]+]] !lgc.rt.shaderstage [[META2:![0-9]+]] !continuation.registercount [[META0:![0-9]+]] !continuation [[META3:![0-9]+]] !continuation.state [[META4:![0-9]+]] {
; MAXPAYLOADSIZE-NEXT:  AllocaSpillBB:
; MAXPAYLOADSIZE-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; MAXPAYLOADSIZE-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_0_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 0
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_1_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 1
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_2_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 2
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_3_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 3
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_4_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 4
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_5_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 5
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_6_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 6
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_7_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 7
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_8_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 8
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_9_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 9
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_10_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 10
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_11_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 11
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_12_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 12
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_13_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 13
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_14_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 14
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_15_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 15
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_16_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 16
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_17_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 17
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_18_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 18
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_19_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 19
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_20_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 20
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_21_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 21
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_22_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 22
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_23_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 23
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_24_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 24
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_25_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 25
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_26_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 26
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_27_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 27
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_28_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 28
; MAXPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_29_EXTRACT:%.*]] = extractvalue [30 x i32] [[PAYLOAD]], 29
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_0_0_0_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0, 0, 0
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_0_1_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0, 1
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_1_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 1
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_0_0_0_EXTRACT136:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0, 0, 0
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_0_1_EXTRACT137:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0, 1
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_1_EXTRACT138:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 1
; MAXPAYLOADSIZE-NEXT:    [[TMP1:%.*]] = icmp eq i32 [[DOTFCA_1_EXTRACT138]], 0
; MAXPAYLOADSIZE-NEXT:    br i1 [[TMP1]], label [[TMP5:%.*]], label [[TMP2:%.*]]
; MAXPAYLOADSIZE:       2:
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_0_0_INSERT:%.*]] = insertvalue [[STRUCT_SYSTEMDATA:%.*]] poison, i32 [[DOTFCA_0_0_0_EXTRACT136]], 0, 0
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_1_INSERT140:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] [[DOTFCA_0_0_INSERT]], float [[DOTFCA_0_1_EXTRACT137]], 1
; MAXPAYLOADSIZE-NEXT:    [[TMP3:%.*]] = call i64 @continuation.getAddrAndMD(ptr @_cont_Traversal)
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_0_INSERT:%.*]] = insertvalue [30 x i32] poison, i32 [[PAYLOAD_FCA_0_EXTRACT]], 0
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_1_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_0_INSERT]], i32 [[PAYLOAD_FCA_1_EXTRACT]], 1
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_2_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_1_INSERT]], i32 [[PAYLOAD_FCA_2_EXTRACT]], 2
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_3_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_2_INSERT]], i32 [[PAYLOAD_FCA_3_EXTRACT]], 3
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_4_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_3_INSERT]], i32 [[PAYLOAD_FCA_4_EXTRACT]], 4
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_5_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_4_INSERT]], i32 [[PAYLOAD_FCA_5_EXTRACT]], 5
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_6_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_5_INSERT]], i32 [[PAYLOAD_FCA_6_EXTRACT]], 6
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_7_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_6_INSERT]], i32 [[PAYLOAD_FCA_7_EXTRACT]], 7
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_8_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_7_INSERT]], i32 [[PAYLOAD_FCA_8_EXTRACT]], 8
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_9_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_8_INSERT]], i32 [[PAYLOAD_FCA_9_EXTRACT]], 9
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_10_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_9_INSERT]], i32 [[PAYLOAD_FCA_10_EXTRACT]], 10
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_11_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_10_INSERT]], i32 [[PAYLOAD_FCA_11_EXTRACT]], 11
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_12_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_11_INSERT]], i32 [[PAYLOAD_FCA_12_EXTRACT]], 12
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_13_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_12_INSERT]], i32 [[PAYLOAD_FCA_13_EXTRACT]], 13
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_14_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_13_INSERT]], i32 [[PAYLOAD_FCA_14_EXTRACT]], 14
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_15_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_14_INSERT]], i32 [[PAYLOAD_FCA_15_EXTRACT]], 15
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_16_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_15_INSERT]], i32 [[PAYLOAD_FCA_16_EXTRACT]], 16
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_17_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_16_INSERT]], i32 [[PAYLOAD_FCA_17_EXTRACT]], 17
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_18_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_17_INSERT]], i32 [[PAYLOAD_FCA_18_EXTRACT]], 18
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_19_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_18_INSERT]], i32 [[PAYLOAD_FCA_19_EXTRACT]], 19
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_20_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_19_INSERT]], i32 [[PAYLOAD_FCA_20_EXTRACT]], 20
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_21_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_20_INSERT]], i32 [[PAYLOAD_FCA_21_EXTRACT]], 21
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_22_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_21_INSERT]], i32 [[PAYLOAD_FCA_22_EXTRACT]], 22
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_23_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_22_INSERT]], i32 [[PAYLOAD_FCA_23_EXTRACT]], 23
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_24_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_23_INSERT]], i32 [[PAYLOAD_FCA_24_EXTRACT]], 24
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_25_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_24_INSERT]], i32 [[PAYLOAD_FCA_25_EXTRACT]], 25
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_26_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_25_INSERT]], i32 [[PAYLOAD_FCA_26_EXTRACT]], 26
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_27_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_26_INSERT]], i32 [[PAYLOAD_FCA_27_EXTRACT]], 27
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_28_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_27_INSERT]], i32 [[PAYLOAD_FCA_28_EXTRACT]], 28
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_29_INSERT:%.*]] = insertvalue [30 x i32] [[DOTFCA_28_INSERT]], i32 [[PAYLOAD_FCA_29_EXTRACT]], 29
; MAXPAYLOADSIZE-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; MAXPAYLOADSIZE-NEXT:    call void (...) @lgc.ilcps.waitContinue(i64 1, i64 -1, i32 [[TMP4]], i64 [[TMP3]], [[STRUCT_SYSTEMDATA]] [[DOTFCA_1_INSERT140]], [9 x i32] poison, [30 x i32] [[DOTFCA_29_INSERT]])
; MAXPAYLOADSIZE-NEXT:    unreachable
; MAXPAYLOADSIZE:       5:
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_0_0_INSERT143:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] poison, i32 [[DOTFCA_0_0_0_EXTRACT136]], 0, 0
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_1_INSERT146:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] [[DOTFCA_0_0_INSERT143]], float [[DOTFCA_0_1_EXTRACT137]], 1
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_0_INSERT3:%.*]] = insertvalue [30 x i32] poison, i32 [[PAYLOAD_FCA_0_EXTRACT]], 0
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_1_INSERT6:%.*]] = insertvalue [30 x i32] [[DOTFCA_0_INSERT3]], i32 [[PAYLOAD_FCA_1_EXTRACT]], 1
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_2_INSERT9:%.*]] = insertvalue [30 x i32] [[DOTFCA_1_INSERT6]], i32 [[PAYLOAD_FCA_2_EXTRACT]], 2
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_3_INSERT12:%.*]] = insertvalue [30 x i32] [[DOTFCA_2_INSERT9]], i32 [[PAYLOAD_FCA_3_EXTRACT]], 3
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_4_INSERT15:%.*]] = insertvalue [30 x i32] [[DOTFCA_3_INSERT12]], i32 [[PAYLOAD_FCA_4_EXTRACT]], 4
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_5_INSERT18:%.*]] = insertvalue [30 x i32] [[DOTFCA_4_INSERT15]], i32 [[PAYLOAD_FCA_5_EXTRACT]], 5
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_6_INSERT21:%.*]] = insertvalue [30 x i32] [[DOTFCA_5_INSERT18]], i32 [[PAYLOAD_FCA_6_EXTRACT]], 6
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_7_INSERT24:%.*]] = insertvalue [30 x i32] [[DOTFCA_6_INSERT21]], i32 [[PAYLOAD_FCA_7_EXTRACT]], 7
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_8_INSERT27:%.*]] = insertvalue [30 x i32] [[DOTFCA_7_INSERT24]], i32 [[PAYLOAD_FCA_8_EXTRACT]], 8
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_9_INSERT30:%.*]] = insertvalue [30 x i32] [[DOTFCA_8_INSERT27]], i32 [[PAYLOAD_FCA_9_EXTRACT]], 9
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_10_INSERT33:%.*]] = insertvalue [30 x i32] [[DOTFCA_9_INSERT30]], i32 [[PAYLOAD_FCA_10_EXTRACT]], 10
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_11_INSERT36:%.*]] = insertvalue [30 x i32] [[DOTFCA_10_INSERT33]], i32 [[PAYLOAD_FCA_11_EXTRACT]], 11
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_12_INSERT39:%.*]] = insertvalue [30 x i32] [[DOTFCA_11_INSERT36]], i32 [[PAYLOAD_FCA_12_EXTRACT]], 12
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_13_INSERT42:%.*]] = insertvalue [30 x i32] [[DOTFCA_12_INSERT39]], i32 [[PAYLOAD_FCA_13_EXTRACT]], 13
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_14_INSERT45:%.*]] = insertvalue [30 x i32] [[DOTFCA_13_INSERT42]], i32 [[PAYLOAD_FCA_14_EXTRACT]], 14
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_15_INSERT48:%.*]] = insertvalue [30 x i32] [[DOTFCA_14_INSERT45]], i32 [[PAYLOAD_FCA_15_EXTRACT]], 15
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_16_INSERT51:%.*]] = insertvalue [30 x i32] [[DOTFCA_15_INSERT48]], i32 [[PAYLOAD_FCA_16_EXTRACT]], 16
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_17_INSERT54:%.*]] = insertvalue [30 x i32] [[DOTFCA_16_INSERT51]], i32 [[PAYLOAD_FCA_17_EXTRACT]], 17
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_18_INSERT57:%.*]] = insertvalue [30 x i32] [[DOTFCA_17_INSERT54]], i32 [[PAYLOAD_FCA_18_EXTRACT]], 18
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_19_INSERT60:%.*]] = insertvalue [30 x i32] [[DOTFCA_18_INSERT57]], i32 [[PAYLOAD_FCA_19_EXTRACT]], 19
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_20_INSERT63:%.*]] = insertvalue [30 x i32] [[DOTFCA_19_INSERT60]], i32 [[PAYLOAD_FCA_20_EXTRACT]], 20
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_21_INSERT66:%.*]] = insertvalue [30 x i32] [[DOTFCA_20_INSERT63]], i32 [[PAYLOAD_FCA_21_EXTRACT]], 21
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_22_INSERT69:%.*]] = insertvalue [30 x i32] [[DOTFCA_21_INSERT66]], i32 [[PAYLOAD_FCA_22_EXTRACT]], 22
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_23_INSERT72:%.*]] = insertvalue [30 x i32] [[DOTFCA_22_INSERT69]], i32 [[PAYLOAD_FCA_23_EXTRACT]], 23
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_24_INSERT75:%.*]] = insertvalue [30 x i32] [[DOTFCA_23_INSERT72]], i32 [[PAYLOAD_FCA_24_EXTRACT]], 24
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_25_INSERT78:%.*]] = insertvalue [30 x i32] [[DOTFCA_24_INSERT75]], i32 [[PAYLOAD_FCA_25_EXTRACT]], 25
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_26_INSERT81:%.*]] = insertvalue [30 x i32] [[DOTFCA_25_INSERT78]], i32 [[PAYLOAD_FCA_26_EXTRACT]], 26
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_27_INSERT84:%.*]] = insertvalue [30 x i32] [[DOTFCA_26_INSERT81]], i32 [[PAYLOAD_FCA_27_EXTRACT]], 27
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_28_INSERT87:%.*]] = insertvalue [30 x i32] [[DOTFCA_27_INSERT84]], i32 [[PAYLOAD_FCA_28_EXTRACT]], 28
; MAXPAYLOADSIZE-NEXT:    [[DOTFCA_29_INSERT90:%.*]] = insertvalue [30 x i32] [[DOTFCA_28_INSERT87]], i32 [[PAYLOAD_FCA_29_EXTRACT]], 29
; MAXPAYLOADSIZE-NEXT:    [[TMP6:%.*]] = load i32, ptr [[CSP]], align 4
; MAXPAYLOADSIZE-NEXT:    call void (...) @lgc.ilcps.waitContinue(i64 0, i64 -1, i32 [[TMP6]], i64 poison, [[STRUCT_SYSTEMDATA]] [[DOTFCA_1_INSERT146]], [9 x i32] poison, [30 x i32] [[DOTFCA_29_INSERT90]])
; MAXPAYLOADSIZE-NEXT:    unreachable
;
;
; PRESERVEDPAYLOADSIZE-LABEL: define void @_cont_Traversal(
; PRESERVEDPAYLOADSIZE-SAME: i32 [[CSPINIT:%.*]], i64 [[RETURNADDR:%.*]], [[STRUCT_TRAVERSALDATA:%.*]] [[TMP0:%.*]], [8 x i32] [[PADDING:%.*]], [4 x i32] [[PAYLOAD:%.*]]) #[[ATTR0:[0-9]+]] !lgc.rt.shaderstage [[META3:![0-9]+]] !continuation.registercount [[META0:![0-9]+]] !continuation [[META4:![0-9]+]] !continuation.state [[META5:![0-9]+]] {
; PRESERVEDPAYLOADSIZE-NEXT:  AllocaSpillBB:
; PRESERVEDPAYLOADSIZE-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; PRESERVEDPAYLOADSIZE-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; PRESERVEDPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_0_EXTRACT:%.*]] = extractvalue [4 x i32] [[PAYLOAD]], 0
; PRESERVEDPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_1_EXTRACT:%.*]] = extractvalue [4 x i32] [[PAYLOAD]], 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_2_EXTRACT:%.*]] = extractvalue [4 x i32] [[PAYLOAD]], 2
; PRESERVEDPAYLOADSIZE-NEXT:    [[PAYLOAD_FCA_3_EXTRACT:%.*]] = extractvalue [4 x i32] [[PAYLOAD]], 3
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_0_0_0_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0, 0, 0
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_0_1_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0, 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_1_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_0_0_0_EXTRACT32:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0, 0, 0
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_0_1_EXTRACT33:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 0, 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_1_EXTRACT34:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[TMP0]], 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[TMP1:%.*]] = icmp eq i32 [[DOTFCA_1_EXTRACT34]], 0
; PRESERVEDPAYLOADSIZE-NEXT:    br i1 [[TMP1]], label [[TMP5:%.*]], label [[TMP2:%.*]]
; PRESERVEDPAYLOADSIZE:       2:
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_0_0_INSERT:%.*]] = insertvalue [[STRUCT_SYSTEMDATA:%.*]] poison, i32 [[DOTFCA_0_0_0_EXTRACT32]], 0, 0
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_1_INSERT36:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] [[DOTFCA_0_0_INSERT]], float [[DOTFCA_0_1_EXTRACT33]], 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[TMP3:%.*]] = call i64 @continuation.getAddrAndMD(ptr @_cont_Traversal)
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_0_INSERT:%.*]] = insertvalue [4 x i32] poison, i32 [[PAYLOAD_FCA_0_EXTRACT]], 0
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_1_INSERT:%.*]] = insertvalue [4 x i32] [[DOTFCA_0_INSERT]], i32 [[PAYLOAD_FCA_1_EXTRACT]], 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_2_INSERT:%.*]] = insertvalue [4 x i32] [[DOTFCA_1_INSERT]], i32 [[PAYLOAD_FCA_2_EXTRACT]], 2
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_3_INSERT:%.*]] = insertvalue [4 x i32] [[DOTFCA_2_INSERT]], i32 [[PAYLOAD_FCA_3_EXTRACT]], 3
; PRESERVEDPAYLOADSIZE-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; PRESERVEDPAYLOADSIZE-NEXT:    call void (...) @lgc.ilcps.waitContinue(i64 1, i64 -1, i32 [[TMP4]], i64 [[TMP3]], [[STRUCT_SYSTEMDATA]] [[DOTFCA_1_INSERT36]], [9 x i32] poison, [4 x i32] [[DOTFCA_3_INSERT]])
; PRESERVEDPAYLOADSIZE-NEXT:    unreachable
; PRESERVEDPAYLOADSIZE:       5:
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_0_0_INSERT39:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] poison, i32 [[DOTFCA_0_0_0_EXTRACT32]], 0, 0
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_1_INSERT42:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] [[DOTFCA_0_0_INSERT39]], float [[DOTFCA_0_1_EXTRACT33]], 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_0_INSERT3:%.*]] = insertvalue [4 x i32] poison, i32 [[PAYLOAD_FCA_0_EXTRACT]], 0
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_1_INSERT6:%.*]] = insertvalue [4 x i32] [[DOTFCA_0_INSERT3]], i32 [[PAYLOAD_FCA_1_EXTRACT]], 1
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_2_INSERT9:%.*]] = insertvalue [4 x i32] [[DOTFCA_1_INSERT6]], i32 [[PAYLOAD_FCA_2_EXTRACT]], 2
; PRESERVEDPAYLOADSIZE-NEXT:    [[DOTFCA_3_INSERT12:%.*]] = insertvalue [4 x i32] [[DOTFCA_2_INSERT9]], i32 [[PAYLOAD_FCA_3_EXTRACT]], 3
; PRESERVEDPAYLOADSIZE-NEXT:    [[TMP6:%.*]] = load i32, ptr [[CSP]], align 4
; PRESERVEDPAYLOADSIZE-NEXT:    call void (...) @lgc.ilcps.waitContinue(i64 0, i64 -1, i32 [[TMP6]], i64 poison, [[STRUCT_SYSTEMDATA]] [[DOTFCA_1_INSERT42]], [9 x i32] poison, [4 x i32] [[DOTFCA_3_INSERT12]])
; PRESERVEDPAYLOADSIZE-NEXT:    unreachable
;
