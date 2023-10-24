; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -passes='dxil-cont-intrinsic-prepare,lint,remove-types-metadata' -S %s 2>%t0.stderr | FileCheck --check-prefix=PREPARE %s
; RUN: count 0 < %t0.stderr
; RUN: opt --verify-each -passes='dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,inline,lint,pre-coroutine-lowering,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,register-buffer,lint,save-continuation-state,lint,dxil-cont-post-process,lint,remove-types-metadata' -S %s 2>%t1.stderr | FileCheck --check-prefix=ALL %s
; RUN: count 0 < %t1.stderr

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.TraversalData = type { %struct.SystemData, i32 }
%struct.SystemData = type { %struct.DispatchSystemData, float }
%struct.DispatchSystemData = type { i32 }

declare i1 @"\01?_AmdContinuationStackIsGlobal@@YA_KXZ"()

declare i32 @"\01?_AmdContPayloadRegistersI32Count@@YA_KXZ"()

declare i32 @"\01?_AmdContPayloadRegistersGetI32@@YA_KXZ"(i32)

declare void @"\01?_AmdContPayloadRegistersSetI32@@YA_KXZ"(i32, i32)

declare !types !0 i32 @"\01?_AmdValueI32CountSomething@@YA_KXZ"(%struct.TraversalData*)

declare !types !2 i32 @"\01?_AmdValueGetI32Something@@YA_KXZ"(%struct.TraversalData*, i32)

declare !types !3 void @"\01?_AmdValueSetI32Something@@YA_KXZ"(%struct.TraversalData*, i32, i32)

; Function Attrs: nounwind
define void @_AmdTraversal(i32 %stackPtr, %struct.TraversalData* %data) #0 !types !4 {
  %1 = getelementptr inbounds %struct.TraversalData, %struct.TraversalData* %data, i32 0, i32 1
  %2 = load i32, i32* %1, align 4
  %3 = icmp eq i32 %2, 0
  %4 = getelementptr inbounds %struct.TraversalData, %struct.TraversalData* %data, i32 0, i32 0
  br i1 %3, label %6, label %5

5:                                                ; preds = %0
  %i0 = call i1 @"\01?_AmdContinuationStackIsGlobal@@YA_KXZ"()
  %i1 = call i32 @"\01?_AmdContPayloadRegistersI32Count@@YA_KXZ"()
  %i2 = call i32 @"\01?_AmdContPayloadRegistersGetI32@@YA_KXZ"(i32 0)
  call void @"\01?_AmdContPayloadRegistersSetI32@@YA_KXZ"(i32 0, i32 1)
  %i3 = call i32 @"\01?_AmdValueI32CountSomething@@YA_KXZ"(%struct.TraversalData* %data)
  %i4 = call i32 @"\01?_AmdValueGetI32Something@@YA_KXZ"(%struct.TraversalData* %data, i32 0)
  call void @"\01?_AmdValueSetI32Something@@YA_KXZ"(%struct.TraversalData* %data, i32 0, i32 1)
  %a0 = zext i1 %i0 to i32
  %a1 = add i32 %a0, %i1
  %a2 = add i32 %a1, %i2
  %a3 = add i32 %a2, %i3
  %a4 = add i32 %a3, %i4
  %addr = zext i32 %a4 to i64
  call void @_AmdWaitEnqueueCall(i64 %addr, i64 -1, i32 %stackPtr, %struct.SystemData* %4) #2
  br label %7

6:                                                ; preds = %0
  call void @_AmdWaitEnqueue(i64 0, i64 -1, i32 %stackPtr, %struct.SystemData* %4) #2
  br label %7

7:                                                ; preds = %6, %5
  ret void
}

declare !types !5 void @_AmdWaitEnqueueCall(i64, i64, i32, %struct.SystemData*) #1

declare !types !5 void @_AmdWaitEnqueue(i64, i64, i32, %struct.SystemData*) #1

attributes #0 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { nounwind }

!0 = !{!"function", i32 poison, !1}
!1 = !{i32 0, %struct.TraversalData poison}
!2 = !{!"function", i32 poison, !1, i32 poison}
!3 = !{!"function", !"void", !1, i32 poison, i32 poison}
!4 = !{!"function", !"void", i32 poison, !1}
!5 = !{!"function", !"void", i64 poison, i64 poison, i32 poison, !6}
!6 = !{i32 0, %struct.SystemData poison}
; PREPARE-LABEL: define void @_AmdTraversal(
; PREPARE-SAME: i32 [[STACKPTR:%.*]], [[STRUCT_TRAVERSALDATA:%.*]] [[DATA:%.*]]) #[[ATTR1:[0-9]+]] {
; PREPARE-NEXT:    [[TMP1:%.*]] = alloca [[STRUCT_TRAVERSALDATA]], align 8
; PREPARE-NEXT:    store [[STRUCT_TRAVERSALDATA]] [[DATA]], ptr [[TMP1]], align 4
; PREPARE-NEXT:    [[TMP2:%.*]] = getelementptr inbounds [[STRUCT_TRAVERSALDATA]], ptr [[TMP1]], i32 0, i32 1
; PREPARE-NEXT:    [[TMP3:%.*]] = load i32, ptr [[TMP2]], align 4
; PREPARE-NEXT:    [[TMP4:%.*]] = icmp eq i32 [[TMP3]], 0
; PREPARE-NEXT:    [[TMP5:%.*]] = getelementptr inbounds [[STRUCT_TRAVERSALDATA]], ptr [[TMP1]], i32 0, i32 0
; PREPARE-NEXT:    br i1 [[TMP4]], label [[TMP13:%.*]], label [[TMP6:%.*]]
; PREPARE:       6:
; PREPARE-NEXT:    [[TMP7:%.*]] = call i1 @_AmdContinuationStackIsGlobal()
; PREPARE-NEXT:    [[TMP8:%.*]] = call i32 @_AmdContPayloadRegistersI32Count()
; PREPARE-NEXT:    [[TMP9:%.*]] = call i32 @_AmdContPayloadRegistersGetI32(i32 0)
; PREPARE-NEXT:    call void @_AmdContPayloadRegistersSetI32(i32 0, i32 1)
; PREPARE-NEXT:    [[TMP10:%.*]] = call i32 @_AmdValueI32CountSomething(ptr [[TMP1]])
; PREPARE-NEXT:    [[TMP11:%.*]] = call i32 @_AmdValueGetI32Something(ptr [[TMP1]], i32 0)
; PREPARE-NEXT:    call void @_AmdValueSetI32Something(ptr [[TMP1]], i32 0, i32 1)
; PREPARE-NEXT:    [[A0:%.*]] = zext i1 [[TMP7]] to i32
; PREPARE-NEXT:    [[A1:%.*]] = add i32 [[A0]], [[TMP8]]
; PREPARE-NEXT:    [[A2:%.*]] = add i32 [[A1]], [[TMP9]]
; PREPARE-NEXT:    [[A3:%.*]] = add i32 [[A2]], [[TMP10]]
; PREPARE-NEXT:    [[A4:%.*]] = add i32 [[A3]], [[TMP11]]
; PREPARE-NEXT:    [[ADDR:%.*]] = zext i32 [[A4]] to i64
; PREPARE-NEXT:    [[TMP12:%.*]] = load [[STRUCT_SYSTEMDATA:%.*]], ptr [[TMP5]], align 4
; PREPARE-NEXT:    call void (i64, i64, ...) @continuation.waitContinue(i64 [[ADDR]], i64 -1, i32 [[STACKPTR]], i64 ptrtoint (ptr @_AmdTraversal to i64), [[STRUCT_SYSTEMDATA]] [[TMP12]])
; PREPARE-NEXT:    br label [[TMP15:%.*]]
; PREPARE:       13:
; PREPARE-NEXT:    [[TMP14:%.*]] = load [[STRUCT_SYSTEMDATA]], ptr [[TMP5]], align 4
; PREPARE-NEXT:    call void (i64, i64, ...) @continuation.waitContinue(i64 0, i64 -1, i32 [[STACKPTR]], [[STRUCT_SYSTEMDATA]] [[TMP14]])
; PREPARE-NEXT:    br label [[TMP15]]
; PREPARE:       15:
; PREPARE-NEXT:    ret void
;
;
; ALL-LABEL: define void @_AmdTraversal(
; ALL-SAME: i32 [[STACKPTR:%.*]], [[STRUCT_TRAVERSALDATA:%.*]] [[DATA:%.*]]) #[[ATTR0:[0-9]+]] !continuation.registercount !0 {
; ALL-NEXT:    [[TMP1:%.*]] = alloca [[STRUCT_TRAVERSALDATA]], align 8
; ALL-NEXT:    [[DATA_FCA_0_0_0_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[DATA]], 0, 0, 0
; ALL-NEXT:    [[DATA_FCA_0_0_0_GEP:%.*]] = getelementptr inbounds [[STRUCT_TRAVERSALDATA]], ptr [[TMP1]], i32 0, i32 0, i32 0, i32 0
; ALL-NEXT:    store i32 [[DATA_FCA_0_0_0_EXTRACT]], ptr [[DATA_FCA_0_0_0_GEP]], align 4
; ALL-NEXT:    [[DATA_FCA_0_1_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[DATA]], 0, 1
; ALL-NEXT:    [[DATA_FCA_0_1_GEP:%.*]] = getelementptr inbounds [[STRUCT_TRAVERSALDATA]], ptr [[TMP1]], i32 0, i32 0, i32 1
; ALL-NEXT:    store float [[DATA_FCA_0_1_EXTRACT]], ptr [[DATA_FCA_0_1_GEP]], align 4
; ALL-NEXT:    [[DATA_FCA_1_EXTRACT:%.*]] = extractvalue [[STRUCT_TRAVERSALDATA]] [[DATA]], 1
; ALL-NEXT:    [[DATA_FCA_1_GEP:%.*]] = getelementptr inbounds [[STRUCT_TRAVERSALDATA]], ptr [[TMP1]], i32 0, i32 1
; ALL-NEXT:    store i32 [[DATA_FCA_1_EXTRACT]], ptr [[DATA_FCA_1_GEP]], align 4
; ALL-NEXT:    [[TMP2:%.*]] = getelementptr inbounds [[STRUCT_TRAVERSALDATA]], ptr [[TMP1]], i32 0, i32 1
; ALL-NEXT:    [[TMP3:%.*]] = load i32, ptr [[TMP2]], align 4
; ALL-NEXT:    [[TMP4:%.*]] = icmp eq i32 [[TMP3]], 0
; ALL-NEXT:    [[TMP5:%.*]] = getelementptr inbounds [[STRUCT_TRAVERSALDATA]], ptr [[TMP1]], i32 0, i32 0
; ALL-NEXT:    br i1 [[TMP4]], label [[TMP12:%.*]], label [[TMP6:%.*]]
; ALL:       6:
; ALL-NEXT:    [[TMP7:%.*]] = load i32, ptr addrspace(20) @REGISTERS, align 4
; ALL-NEXT:    store i32 1, ptr addrspace(20) @REGISTERS, align 4
; ALL-NEXT:    [[TMP8:%.*]] = getelementptr i32, ptr [[TMP1]], i32 0
; ALL-NEXT:    [[TMP9:%.*]] = load i32, ptr [[TMP8]], align 4
; ALL-NEXT:    [[TMP10:%.*]] = getelementptr i32, ptr [[TMP1]], i32 0
; ALL-NEXT:    store i32 1, ptr [[TMP10]], align 4
; ALL-NEXT:    [[A0:%.*]] = zext i1 false to i32
; ALL-NEXT:    [[A1:%.*]] = add i32 [[A0]], 30
; ALL-NEXT:    [[A2:%.*]] = add i32 [[A1]], [[TMP7]]
; ALL-NEXT:    [[A3:%.*]] = add i32 [[A2]], 3
; ALL-NEXT:    [[A4:%.*]] = add i32 [[A3]], [[TMP9]]
; ALL-NEXT:    [[ADDR:%.*]] = zext i32 [[A4]] to i64
; ALL-NEXT:    [[DOTFCA_0_0_GEP:%.*]] = getelementptr inbounds [[STRUCT_SYSTEMDATA:%.*]], ptr [[TMP5]], i32 0, i32 0, i32 0
; ALL-NEXT:    [[DOTFCA_0_0_LOAD:%.*]] = load i32, ptr [[DOTFCA_0_0_GEP]], align 4
; ALL-NEXT:    [[DOTFCA_0_0_INSERT:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] poison, i32 [[DOTFCA_0_0_LOAD]], 0, 0
; ALL-NEXT:    [[DOTFCA_1_GEP:%.*]] = getelementptr inbounds [[STRUCT_SYSTEMDATA]], ptr [[TMP5]], i32 0, i32 1
; ALL-NEXT:    [[DOTFCA_1_LOAD:%.*]] = load float, ptr [[DOTFCA_1_GEP]], align 4
; ALL-NEXT:    [[DOTFCA_1_INSERT:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] [[DOTFCA_0_0_INSERT]], float [[DOTFCA_1_LOAD]], 1
; ALL-NEXT:    [[TMP11:%.*]] = call i64 @continuation.getAddrAndMD(i64 ptrtoint (ptr @_AmdTraversal to i64))
; ALL-NEXT:    call void (i64, i64, ...) @continuation.waitContinue(i64 [[ADDR]], i64 -1, i32 [[STACKPTR]], i64 [[TMP11]], [[STRUCT_SYSTEMDATA]] [[DOTFCA_1_INSERT]]), !continuation.registercount !0
; ALL-NEXT:    br label [[TMP13:%.*]]
; ALL:       12:
; ALL-NEXT:    [[DOTFCA_0_0_GEP1:%.*]] = getelementptr inbounds [[STRUCT_SYSTEMDATA]], ptr [[TMP5]], i32 0, i32 0, i32 0
; ALL-NEXT:    [[DOTFCA_0_0_LOAD2:%.*]] = load i32, ptr [[DOTFCA_0_0_GEP1]], align 4
; ALL-NEXT:    [[DOTFCA_0_0_INSERT3:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] poison, i32 [[DOTFCA_0_0_LOAD2]], 0, 0
; ALL-NEXT:    [[DOTFCA_1_GEP4:%.*]] = getelementptr inbounds [[STRUCT_SYSTEMDATA]], ptr [[TMP5]], i32 0, i32 1
; ALL-NEXT:    [[DOTFCA_1_LOAD5:%.*]] = load float, ptr [[DOTFCA_1_GEP4]], align 4
; ALL-NEXT:    [[DOTFCA_1_INSERT6:%.*]] = insertvalue [[STRUCT_SYSTEMDATA]] [[DOTFCA_0_0_INSERT3]], float [[DOTFCA_1_LOAD5]], 1
; ALL-NEXT:    call void (i64, i64, ...) @continuation.waitContinue(i64 0, i64 -1, i32 [[STACKPTR]], [[STRUCT_SYSTEMDATA]] [[DOTFCA_1_INSERT6]]), !continuation.registercount !0
; ALL-NEXT:    br label [[TMP13]]
; ALL:       13:
; ALL-NEXT:    ret void
;
