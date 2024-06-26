; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --check-globals --version 3
; RUN: opt --verify-each -passes='legacy-cleanup-continuations,lint' -S %s --lint-abort-on-error | FileCheck %s

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%continuation.token = type { }
%await_with_ret_value.Frame = type { i64 }
%simple_await.Frame = type { i64 }
%simple_await_entry.Frame = type { }

declare %continuation.token* @async_fun()
declare i32 @lgc.ilcps.getReturnValue__i32() #0
declare void @lgc.ilcps.return(i64, ...)

define { i8*, %continuation.token* } @simple_await(i64 %dummyRet, i8* %0) !continuation !0 !continuation.registercount !4 {
; CHECK-LABEL: define void @simple_await(
; CHECK-SAME: i64 [[DUMMYRET:%.*]]) !continuation [[META1:![0-9]+]] !continuation.registercount [[META2:![0-9]+]] !continuation.stacksize [[META3:![0-9]+]] !continuation.state [[META3]] {
; CHECK-NEXT:  AllocaSpillBB:
; CHECK-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 8)
; CHECK-NEXT:    [[FRAMEPTR:%.*]] = bitcast ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]] to ptr addrspace(32)
; CHECK-NEXT:    [[DOTSPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME:%.*]], ptr addrspace(32) [[FRAMEPTR]], i32 0, i32 0
; CHECK-NEXT:    store i64 -1, ptr addrspace(32) [[DOTSPILL_ADDR]], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = call i64 (...) @lgc.cps.as.continuation.reference__i64(ptr @simple_await.resume.0)
; CHECK-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i64 [[TMP0]]), !continuation.registercount [[META2]], !continuation.returnedRegistercount [[META2]]
; CHECK-NEXT:    unreachable
;
AllocaSpillBB:
  %FramePtr = bitcast i8* %0 to %simple_await.Frame*
  %.spill.addr = getelementptr inbounds %simple_await.Frame, %simple_await.Frame* %FramePtr, i32 0, i32 0
  store i64 -1, i64* %.spill.addr, align 4
  %tok = call %continuation.token* @async_fun(), !continuation.registercount !4, !continuation.returnedRegistercount !4
  %1 = insertvalue { i8*, %continuation.token* } { i8* bitcast ({ i8*, %continuation.token* } (i8*, i1)* @simple_await.resume.0 to i8*), %continuation.token* undef }, %continuation.token* %tok, 1
  ret { i8*, %continuation.token* } %1
}

define internal { i8*, %continuation.token* } @simple_await.resume.0(i8* noalias nonnull align 16 dereferenceable(8) %0, i1 %1) !continuation !0 {
; CHECK-LABEL: define dso_local void @simple_await.resume.0(
; CHECK-SAME: i64 [[TMP0:%.*]]) !continuation [[META1]] !continuation.registercount [[META2]] {
; CHECK-NEXT:  entryresume.0:
; CHECK-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 8)
; CHECK-NEXT:    [[FRAMEPTR:%.*]] = bitcast ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]] to ptr addrspace(32)
; CHECK-NEXT:    [[VFRAME:%.*]] = bitcast ptr addrspace(32) [[FRAMEPTR]] to ptr addrspace(32)
; CHECK-NEXT:    [[DOTRELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME:%.*]], ptr addrspace(32) [[FRAMEPTR]], i32 0, i32 0
; CHECK-NEXT:    [[DOTRELOAD:%.*]] = load i64, ptr addrspace(32) [[DOTRELOAD_ADDR]], align 4
; CHECK-NEXT:    call void @lgc.cps.free(i32 8)
; CHECK-NEXT:    call void (i64, ...) @continuation.continue(i64 [[DOTRELOAD]], i64 poison, i64 undef), !continuation.registercount [[META2]]
; CHECK-NEXT:    unreachable
;
entryresume.0:
  %FramePtr = bitcast i8* %0 to %simple_await.Frame*
  %vFrame = bitcast %simple_await.Frame* %FramePtr to i8*
  %.reload.addr = getelementptr inbounds %simple_await.Frame, %simple_await.Frame* %FramePtr, i32 0, i32 0
  %.reload = load i64, i64* %.reload.addr, align 4
  call void (i64, ...) @lgc.ilcps.return(i64 %.reload, i64 undef), !continuation.registercount !4
  unreachable
}

define { i8*, %continuation.token* } @simple_await_entry(i64 %dummyRet, i8* %0) !continuation.entry !2 !continuation !3 !continuation.registercount !4 {
; CHECK-LABEL: define void @simple_await_entry(
; CHECK-SAME: i64 [[DUMMYRET:%.*]]) !continuation [[META4:![0-9]+]] !continuation.registercount [[META2]] !continuation.entry [[META5:![0-9]+]] !continuation.stacksize [[META3]] !continuation.state [[META3]] {
; CHECK-NEXT:  AllocaSpillBB:
; CHECK-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 8)
; CHECK-NEXT:    [[FRAMEPTR:%.*]] = bitcast ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]] to ptr addrspace(32)
; CHECK-NEXT:    [[TMP0:%.*]] = call i64 (...) @lgc.cps.as.continuation.reference__i64(ptr @simple_await_entry.resume.0)
; CHECK-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i64 [[TMP0]]), !continuation.registercount [[META2]], !continuation.returnedRegistercount [[META2]]
; CHECK-NEXT:    unreachable
;
AllocaSpillBB:
  %FramePtr = bitcast i8* %0 to %simple_await_entry.Frame*
  %tok = call %continuation.token* @async_fun(), !continuation.registercount !4, !continuation.returnedRegistercount !4
  %1 = bitcast { i8*, %continuation.token* } (i8*, i1)* @simple_await_entry.resume.0 to i8*
  %2 = insertvalue { i8*, %continuation.token* } undef, i8* %1, 0
  %3 = insertvalue { i8*, %continuation.token* } %2, %continuation.token* %tok, 1
  ret { i8*, %continuation.token* } %3
}

define internal { i8*, %continuation.token* } @simple_await_entry.resume.0(i8* noalias nonnull align 16 dereferenceable(8) %0, i1 %1) !continuation.entry !2 !continuation !3 {
; CHECK-LABEL: define dso_local void @simple_await_entry.resume.0(
; CHECK-SAME: i64 [[TMP0:%.*]]) !continuation [[META4]] !continuation.registercount [[META2]] {
; CHECK-NEXT:  entryresume.0:
; CHECK-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 8)
; CHECK-NEXT:    [[FRAMEPTR:%.*]] = bitcast ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]] to ptr addrspace(32)
; CHECK-NEXT:    [[VFRAME:%.*]] = bitcast ptr addrspace(32) [[FRAMEPTR]] to ptr addrspace(32)
; CHECK-NEXT:    call void @lgc.cps.free(i32 8)
; CHECK-NEXT:    ret void
; CHECK:       entryresume.0.split:
; CHECK-NEXT:    unreachable
;
entryresume.0:
  %FramePtr = bitcast i8* %0 to %simple_await_entry.Frame*
  %vFrame = bitcast %simple_await_entry.Frame* %FramePtr to i8*
  call void (i64, ...) @lgc.ilcps.return(i64 undef), !continuation.registercount !4
  unreachable
}

define { i8*, %continuation.token* } @await_with_ret_value(i64 %dummyRet, i8* %0) !continuation !1 !continuation.registercount !4 {
; CHECK-LABEL: define void @await_with_ret_value(
; CHECK-SAME: i64 [[DUMMYRET:%.*]]) !continuation [[META6:![0-9]+]] !continuation.registercount [[META2]] !continuation.stacksize [[META3]] !continuation.state [[META3]] {
; CHECK-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 8)
; CHECK-NEXT:    [[FRAMEPTR:%.*]] = bitcast ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]] to ptr addrspace(32)
; CHECK-NEXT:    [[DOTSPILL_ADDR:%.*]] = getelementptr inbounds [[AWAIT_WITH_RET_VALUE_FRAME:%.*]], ptr addrspace(32) [[FRAMEPTR]], i32 0, i32 0
; CHECK-NEXT:    store i64 -1, ptr addrspace(32) [[DOTSPILL_ADDR]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = call i64 (...) @lgc.cps.as.continuation.reference__i64(ptr @await_with_ret_value.resume.0)
; CHECK-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i64 [[TMP1]]), !continuation.registercount [[META2]], !continuation.returnedRegistercount [[META2]]
; CHECK-NEXT:    unreachable
;
  %FramePtr = bitcast i8* %0 to %await_with_ret_value.Frame*
  %.spill.addr = getelementptr inbounds %await_with_ret_value.Frame, %await_with_ret_value.Frame* %FramePtr, i32 0, i32 0
  store i64 -1, i64* %.spill.addr, align 4
  %tok = call %continuation.token* @async_fun(), !continuation.registercount !4, !continuation.returnedRegistercount !4
  %res = insertvalue { i8*, %continuation.token* } { i8* bitcast ({ i8*, %continuation.token* } (i8*, i1)* @await_with_ret_value.resume.0 to i8*), %continuation.token* undef }, %continuation.token* %tok, 1
  ret { i8*, %continuation.token* } %res
}

define internal { i8*, %continuation.token* } @await_with_ret_value.resume.0(i8* noalias nonnull align 16 dereferenceable(8) %0, i1 %1) !continuation !1 {
; CHECK-LABEL: define dso_local void @await_with_ret_value.resume.0(
; CHECK-SAME: i64 [[TMP0:%.*]], i32 [[RES1:%.*]]) !continuation [[META6]] !continuation.registercount [[META2]] {
; CHECK-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 8)
; CHECK-NEXT:    [[FRAMEPTR:%.*]] = bitcast ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]] to ptr addrspace(32)
; CHECK-NEXT:    [[VFRAME:%.*]] = bitcast ptr addrspace(32) [[FRAMEPTR]] to ptr addrspace(32)
; CHECK-NEXT:    [[DOTRELOAD_ADDR:%.*]] = getelementptr inbounds [[AWAIT_WITH_RET_VALUE_FRAME:%.*]], ptr addrspace(32) [[FRAMEPTR]], i32 0, i32 0
; CHECK-NEXT:    [[DOTRELOAD:%.*]] = load i64, ptr addrspace(32) [[DOTRELOAD_ADDR]], align 4
; CHECK-NEXT:    call void @lgc.cps.free(i32 8)
; CHECK-NEXT:    call void (i64, ...) @continuation.continue(i64 [[DOTRELOAD]], i64 poison, i32 [[RES1]], i64 undef), !continuation.registercount [[META2]]
; CHECK-NEXT:    unreachable
;
  %FramePtr = bitcast i8* %0 to %await_with_ret_value.Frame*
  %vFrame = bitcast %await_with_ret_value.Frame* %FramePtr to i8*
  %.reload.addr = getelementptr inbounds %await_with_ret_value.Frame, %await_with_ret_value.Frame* %FramePtr, i32 0, i32 0
  %.reload = load i64, i64* %.reload.addr, align 4
  %res = call i32 @lgc.ilcps.getReturnValue__i32()
  call void (i64, ...) @lgc.ilcps.return(i64 %.reload, i32 %res, i64 undef), !continuation.registercount !4
  unreachable
}

attributes #0 = { nounwind }

!continuation.stackAddrspace = !{!5}

!0 = !{{ i8*, %continuation.token* } (i8*)* @simple_await}
!1 = !{{ i8*, %continuation.token* } (i8*)* @await_with_ret_value}
!2 = !{}
!3 = !{{ i8*, %continuation.token* } (i8*)* @simple_await_entry}
!4 = !{i32 0}
!5 = !{i32 21}
;.
; CHECK: attributes #[[ATTR0:[0-9]+]] = { nounwind }
; CHECK: attributes #[[ATTR1:[0-9]+]] = { noreturn }
; CHECK: attributes #[[ATTR2:[0-9]+]] = { nounwind willreturn memory(inaccessiblemem: readwrite) }
; CHECK: attributes #[[ATTR3:[0-9]+]] = { nounwind willreturn }
; CHECK: attributes #[[ATTR4:[0-9]+]] = { nounwind willreturn memory(inaccessiblemem: read) }
;.
; CHECK: [[META0:![0-9]+]] = !{i32 21}
; CHECK: [[META1]] = !{ptr @simple_await}
; CHECK: [[META2]] = !{i32 0}
; CHECK: [[META3]] = !{i32 8}
; CHECK: [[META4]] = !{ptr @simple_await_entry}
; CHECK: [[META5]] = !{}
; CHECK: [[META6]] = !{ptr @await_with_ret_value}
;.
