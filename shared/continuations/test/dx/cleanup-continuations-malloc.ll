; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 3
; RUN: opt --verify-each -passes='lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint' -S %s 2> %t.stderr | FileCheck %s
; RUN: count 0 < %t.stderr

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%continuation.token = type { }

declare void @await.void(%continuation.token*)
declare %continuation.token* @async_fun()

define <4 x i32> @simple_await(<4 x i32> %arg) !continuation.registercount !1 {
; CHECK-LABEL: define void @simple_await(
; CHECK-SAME: i32 [[CSPINIT:%.*]], i64 [[RETURNADDR:%.*]], <4 x i32> [[ARG:%.*]]) !continuation.registercount [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] !continuation.state [[META3:![0-9]+]] !continuation.stacksize [[META3]] {
; CHECK-NEXT:  AllocaSpillBB:
; CHECK-NEXT:    [[TMP0:%.*]] = call ptr @continuation.getContinuationStackOffset()
; CHECK-NEXT:    [[TMP1:%.*]] = load i32, ptr [[TMP0]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[TMP1]] to ptr addrspace(21)
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(21) [[TMP2]], i64 0
; CHECK-NEXT:    [[ARG_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME:%.*]], ptr addrspace(21) [[TMP3]], i32 0, i32 0
; CHECK-NEXT:    store <4 x i32> [[ARG]], ptr addrspace(21) [[ARG_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[RETURNADDR_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME]], ptr addrspace(21) [[TMP3]], i32 0, i32 1
; CHECK-NEXT:    store i64 [[RETURNADDR]], ptr addrspace(21) [[RETURNADDR_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = call ptr @continuation.getContinuationStackOffset()
; CHECK-NEXT:    [[TMP5:%.*]] = load i32, ptr [[TMP4]], align 4
; CHECK-NEXT:    [[TMP6:%.*]] = add i32 [[TMP5]], 24
; CHECK-NEXT:    store i32 [[TMP6]], ptr [[TMP4]], align 4
; CHECK-NEXT:    [[TMP7:%.*]] = call ptr @continuation.getContinuationStackOffset()
; CHECK-NEXT:    [[TMP8:%.*]] = load i32, ptr [[TMP7]], align 4
; CHECK-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i32 [[TMP8]], i64 ptrtoint (ptr @simple_await.resume.0 to i64)), !continuation.registercount [[META1]], !continuation.returnedRegistercount !1
; CHECK-NEXT:    unreachable
;
  %tok = call %continuation.token* @async_fun(), !continuation.registercount !1, !continuation.returnedRegistercount !1
  call void @await.void(%continuation.token* %tok)
  ret <4 x i32> %arg, !continuation.registercount !1
}

define void @simple_await_entry(<4 x i32> %arg, <4 x i32> addrspace(1)* %mem) !continuation.entry !0 !continuation.registercount !1 {
; CHECK-LABEL: define void @simple_await_entry(
; CHECK-SAME: <4 x i32> [[ARG:%.*]], ptr addrspace(1) [[MEM:%.*]]) !continuation.registercount [[META1]] !continuation.entry [[META4:![0-9]+]] !continuation [[META5:![0-9]+]] !continuation.state [[META3]] !continuation.stacksize [[META3]] {
; CHECK-NEXT:  AllocaSpillBB:
; CHECK-NEXT:    [[TMP0:%.*]] = call ptr @continuation.getContinuationStackOffset()
; CHECK-NEXT:    [[TMP1:%.*]] = load i32, ptr [[TMP0]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[TMP1]] to ptr addrspace(21)
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(21) [[TMP2]], i64 0
; CHECK-NEXT:    [[MEM_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME:%.*]], ptr addrspace(21) [[TMP3]], i32 0, i32 1
; CHECK-NEXT:    store ptr addrspace(1) [[MEM]], ptr addrspace(21) [[MEM_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[ARG_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME]], ptr addrspace(21) [[TMP3]], i32 0, i32 0
; CHECK-NEXT:    store <4 x i32> [[ARG]], ptr addrspace(21) [[ARG_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = call ptr @continuation.getContinuationStackOffset()
; CHECK-NEXT:    [[TMP5:%.*]] = load i32, ptr [[TMP4]], align 4
; CHECK-NEXT:    [[TMP6:%.*]] = add i32 [[TMP5]], 24
; CHECK-NEXT:    store i32 [[TMP6]], ptr [[TMP4]], align 4
; CHECK-NEXT:    [[TMP7:%.*]] = call ptr @continuation.getContinuationStackOffset()
; CHECK-NEXT:    [[TMP8:%.*]] = load i32, ptr [[TMP7]], align 4
; CHECK-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i32 [[TMP8]], i64 ptrtoint (ptr @simple_await_entry.resume.0 to i64)), !continuation.registercount [[META1]], !continuation.returnedRegistercount !1
; CHECK-NEXT:    unreachable
;
  %tok = call %continuation.token* @async_fun(), !continuation.registercount !1, !continuation.returnedRegistercount !1
  call void @await.void(%continuation.token* %tok)
  store <4 x i32> %arg, <4 x i32> addrspace(1)* %mem
  ret void, !continuation.registercount !1
}

!continuation.stackAddrspace = !{!2}

!0 = !{}
!1 = !{i32 0}
!2 = !{i32 21}
