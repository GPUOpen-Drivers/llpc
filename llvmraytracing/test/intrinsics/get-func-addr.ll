; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 2
; RUN: opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint<abort-on-error>,lower-raytracing-pipeline,lint<abort-on-error>' -S %s | FileCheck %s
;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 ;
 ;  Permission is hereby granted, free of charge, to any person obtaining a copy
 ;  of this software and associated documentation files (the "Software"), to
 ;  deal in the Software without restriction, including without limitation the
 ;  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 ;  sell copies of the Software, and to permit persons to whom the Software is
 ;  furnished to do so, subject to the following conditions:
 ;
 ;  The above copyright notice and this permission notice shall be included in all
 ;  copies or substantial portions of the Software.
 ;
 ;  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ;  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ;  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 ;  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 ;  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 ;  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 ;  IN THE SOFTWARE.
 ;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

%struct.DispatchSystemData = type { i32 }

declare i32 @_AmdGetFuncAddrMyFunc()

%struct.TraversalData = type { }

declare !pointeetys !12 i1 @_cont_ReportHit(%struct.TraversalData* %data, float %t, i32 %hitKind)

declare !pointeetys !{%struct.DispatchSystemData poison} void @_cont_DispatchRaysIndex3(%struct.DispatchSystemData*)

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !8 {
  ret void
}

define { i32, i32 } @main() !lgc.rt.shaderstage !10 {
; CHECK-LABEL: define void @main
; CHECK-SAME: (i32 [[SHADERINDEX:%.*]], i32 [[RETURNADDR:%.*]], [[STRUCT_DISPATCHSYSTEMDATA:%.*]] [[TMP0:%.*]], {} [[TMP1:%.*]], [0 x i32] [[TMP2:%.*]], [0 x i32] [[TMP3:%.*]]) !lgc.rt.shaderstage [[META5:![0-9]+]] !lgc.cps [[META11:![0-9]+]] !continuation [[META12:![0-9]+]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; CHECK-NEXT:    [[PAYLOAD_SERIALIZATION_ALLOCA:%.*]] = alloca [0 x i32], align 4
; CHECK-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP0]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; CHECK-NEXT:    call void @lgc.ilcps.setLocalRootIndex(i32 0)
; CHECK-NEXT:    [[TMP4:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @MyFunc)
; CHECK-NEXT:    [[V0:%.*]] = insertvalue { i32, i32 } undef, i32 [[TMP4]], 0
; CHECK-NEXT:    call void @lgc.cps.complete()
; CHECK-NEXT:    unreachable
;
entry:
  %val = call i32 @_AmdGetFuncAddrMyFunc()
  %v0 = insertvalue { i32, i32 } undef, i32 %val, 0
  ret { i32, i32 } %v0
}

define i32 @MyFunc() {
; CHECK-LABEL: define i32 @MyFunc() {
; CHECK-NEXT:    ret i32 5
;
  ret i32 5
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
!10 = !{i32 0}
!11 = !{i32 0, %struct.TraversalData poison}
!12 = !{%struct.TraversalData poison}

