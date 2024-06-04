; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -passes='lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint' -S %s --lint-abort-on-error | FileCheck -check-prefix=CLEANUP %s
; RUN: opt --verify-each -passes='lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,register-buffer,lint' \
; RUN:     -S %s --lint-abort-on-error | FileCheck -check-prefix=REGISTERBUFFER %s

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%continuation.token = type { }

declare void @await.void(%continuation.token*)
declare i32 @_cont_GetContinuationStackAddr()
declare %continuation.token* @async_fun()

@PAYLOAD = external addrspace(20) global [30 x i32]

define <4 x i32> @simple_await(i64 %returnAddr, <4 x i32> %arg) !continuation.registercount !1 {
  %tok = call %continuation.token* @async_fun(), !continuation.registercount !1, !continuation.returnedRegistercount !1
  call void @await.void(%continuation.token* %tok)
  ret <4 x i32> %arg, !continuation.registercount !1
}

define void @simple_await_entry(i64 %returnAddr, <4 x i32> %arg, <4 x i32> addrspace(1)* %mem) !continuation.entry !0 !continuation.registercount !1 {
  %tok = call %continuation.token* @async_fun(), !continuation.registercount !1, !continuation.returnedRegistercount !1
  call void @await.void(%continuation.token* %tok)
  store <4 x i32> %arg, <4 x i32> addrspace(1)* %mem
  ret void, !continuation.registercount !1
}

!continuation.maxPayloadRegisterCount = !{!2}
!continuation.stackAddrspace = !{!3}

!0 = !{}
!1 = !{i32 0}
!2 = !{i32 30}
!3 = !{i32 21}
; CLEANUP-LABEL: define void @simple_await(
; CLEANUP-SAME: i64 [[RETURNADDR:%.*]], <4 x i32> [[ARG:%.*]]) !continuation.registercount [[META2:![0-9]+]] !continuation [[META3:![0-9]+]] !continuation.stacksize [[META4:![0-9]+]] !continuation.state [[META4]] {
; CLEANUP-NEXT:  AllocaSpillBB:
; CLEANUP-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 24)
; CLEANUP-NEXT:    [[ARG_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; CLEANUP-NEXT:    store <4 x i32> [[ARG]], ptr addrspace(32) [[ARG_SPILL_ADDR]], align 4
; CLEANUP-NEXT:    [[RETURNADDR_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; CLEANUP-NEXT:    store i64 [[RETURNADDR]], ptr addrspace(32) [[RETURNADDR_SPILL_ADDR]], align 4
; CLEANUP-NEXT:    [[TMP0:%.*]] = call i64 (...) @lgc.cps.as.continuation.reference__i64(ptr @simple_await.resume.0)
; CLEANUP-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i64 [[TMP0]]), !continuation.registercount [[META2]], !continuation.returnedRegistercount [[META2]]
; CLEANUP-NEXT:    unreachable
;
;
; CLEANUP-LABEL: define dso_local void @simple_await.resume.0(
; CLEANUP-SAME: i64 [[TMP0:%.*]]) !continuation.registercount [[META2]] !continuation [[META3]] {
; CLEANUP-NEXT:  entryresume.0:
; CLEANUP-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 24)
; CLEANUP-NEXT:    [[ARG_RELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; CLEANUP-NEXT:    [[ARG_RELOAD:%.*]] = load <4 x i32>, ptr addrspace(32) [[ARG_RELOAD_ADDR]], align 4
; CLEANUP-NEXT:    [[RETURNADDR_RELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; CLEANUP-NEXT:    [[RETURNADDR_RELOAD:%.*]] = load i64, ptr addrspace(32) [[RETURNADDR_RELOAD_ADDR]], align 4
; CLEANUP-NEXT:    call void @lgc.cps.free(i32 24)
; CLEANUP-NEXT:    call void (i64, ...) @continuation.continue(i64 [[RETURNADDR_RELOAD]], i64 poison, <4 x i32> [[ARG_RELOAD]]), !continuation.registercount [[META2]]
; CLEANUP-NEXT:    unreachable
;
;
; CLEANUP-LABEL: define void @simple_await_entry(
; CLEANUP-SAME: i64 [[RETURNADDR:%.*]], <4 x i32> [[ARG:%.*]], ptr addrspace(1) [[MEM:%.*]]) !continuation.registercount [[META2]] !continuation.entry [[META5:![0-9]+]] !continuation [[META6:![0-9]+]] !continuation.stacksize [[META4]] !continuation.state [[META4]] {
; CLEANUP-NEXT:  AllocaSpillBB:
; CLEANUP-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 24)
; CLEANUP-NEXT:    [[MEM_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; CLEANUP-NEXT:    store ptr addrspace(1) [[MEM]], ptr addrspace(32) [[MEM_SPILL_ADDR]], align 4
; CLEANUP-NEXT:    [[ARG_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; CLEANUP-NEXT:    store <4 x i32> [[ARG]], ptr addrspace(32) [[ARG_SPILL_ADDR]], align 4
; CLEANUP-NEXT:    [[TMP0:%.*]] = call i64 (...) @lgc.cps.as.continuation.reference__i64(ptr @simple_await_entry.resume.0)
; CLEANUP-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i64 [[TMP0]]), !continuation.registercount [[META2]], !continuation.returnedRegistercount [[META2]]
; CLEANUP-NEXT:    unreachable
;
;
; CLEANUP-LABEL: define dso_local void @simple_await_entry.resume.0(
; CLEANUP-SAME: i64 [[TMP0:%.*]]) !continuation.registercount [[META2]] !continuation [[META6]] {
; CLEANUP-NEXT:  entryresume.0:
; CLEANUP-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 24)
; CLEANUP-NEXT:    [[MEM_RELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; CLEANUP-NEXT:    [[MEM_RELOAD:%.*]] = load ptr addrspace(1), ptr addrspace(32) [[MEM_RELOAD_ADDR]], align 4
; CLEANUP-NEXT:    [[ARG_RELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; CLEANUP-NEXT:    [[ARG_RELOAD:%.*]] = load <4 x i32>, ptr addrspace(32) [[ARG_RELOAD_ADDR]], align 4
; CLEANUP-NEXT:    store <4 x i32> [[ARG_RELOAD]], ptr addrspace(1) [[MEM_RELOAD]], align 4
; CLEANUP-NEXT:    call void @lgc.cps.free(i32 24)
; CLEANUP-NEXT:    ret void
; CLEANUP:       entryresume.0.split:
; CLEANUP-NEXT:    unreachable
;
;
; REGISTERBUFFER-LABEL: define void @simple_await(
; REGISTERBUFFER-SAME: i64 [[RETURNADDR:%.*]], <4 x i32> [[ARG:%.*]]) !continuation.registercount [[META2:![0-9]+]] !continuation [[META3:![0-9]+]] !continuation.stacksize [[META4:![0-9]+]] !continuation.state [[META4]] {
; REGISTERBUFFER-NEXT:  AllocaSpillBB:
; REGISTERBUFFER-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 24)
; REGISTERBUFFER-NEXT:    [[ARG_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; REGISTERBUFFER-NEXT:    store <4 x i32> [[ARG]], ptr addrspace(32) [[ARG_SPILL_ADDR]], align 4
; REGISTERBUFFER-NEXT:    [[RETURNADDR_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; REGISTERBUFFER-NEXT:    store i64 [[RETURNADDR]], ptr addrspace(32) [[RETURNADDR_SPILL_ADDR]], align 4
; REGISTERBUFFER-NEXT:    [[TMP0:%.*]] = call i64 (...) @lgc.cps.as.continuation.reference__i64(ptr @simple_await.resume.0)
; REGISTERBUFFER-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i64 [[TMP0]]), !continuation.registercount [[META2]], !continuation.returnedRegistercount [[META2]]
; REGISTERBUFFER-NEXT:    unreachable
;
;
; REGISTERBUFFER-LABEL: define dso_local void @simple_await.resume.0(
; REGISTERBUFFER-SAME: i64 [[TMP0:%.*]]) !continuation.registercount [[META2]] !continuation [[META3]] {
; REGISTERBUFFER-NEXT:  entryresume.0:
; REGISTERBUFFER-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 24)
; REGISTERBUFFER-NEXT:    [[ARG_RELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; REGISTERBUFFER-NEXT:    [[ARG_RELOAD:%.*]] = load <4 x i32>, ptr addrspace(32) [[ARG_RELOAD_ADDR]], align 4
; REGISTERBUFFER-NEXT:    [[RETURNADDR_RELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; REGISTERBUFFER-NEXT:    [[RETURNADDR_RELOAD:%.*]] = load i64, ptr addrspace(32) [[RETURNADDR_RELOAD_ADDR]], align 4
; REGISTERBUFFER-NEXT:    call void @lgc.cps.free(i32 24)
; REGISTERBUFFER-NEXT:    call void (i64, ...) @continuation.continue(i64 [[RETURNADDR_RELOAD]], i64 poison, <4 x i32> [[ARG_RELOAD]]), !continuation.registercount [[META2]]
; REGISTERBUFFER-NEXT:    unreachable
;
;
; REGISTERBUFFER-LABEL: define void @simple_await_entry(
; REGISTERBUFFER-SAME: i64 [[RETURNADDR:%.*]], <4 x i32> [[ARG:%.*]], ptr addrspace(1) [[MEM:%.*]]) !continuation.registercount [[META2]] !continuation.entry [[META5:![0-9]+]] !continuation [[META6:![0-9]+]] !continuation.stacksize [[META4]] !continuation.state [[META4]] {
; REGISTERBUFFER-NEXT:  AllocaSpillBB:
; REGISTERBUFFER-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 24)
; REGISTERBUFFER-NEXT:    [[MEM_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; REGISTERBUFFER-NEXT:    store ptr addrspace(1) [[MEM]], ptr addrspace(32) [[MEM_SPILL_ADDR]], align 4
; REGISTERBUFFER-NEXT:    [[ARG_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; REGISTERBUFFER-NEXT:    store <4 x i32> [[ARG]], ptr addrspace(32) [[ARG_SPILL_ADDR]], align 4
; REGISTERBUFFER-NEXT:    [[TMP0:%.*]] = call i64 (...) @lgc.cps.as.continuation.reference__i64(ptr @simple_await_entry.resume.0)
; REGISTERBUFFER-NEXT:    call void (i64, ...) @continuation.continue(i64 ptrtoint (ptr @async_fun to i64), i64 [[TMP0]]), !continuation.registercount [[META2]], !continuation.returnedRegistercount [[META2]]
; REGISTERBUFFER-NEXT:    unreachable
;
;
; REGISTERBUFFER-LABEL: define dso_local void @simple_await_entry.resume.0(
; REGISTERBUFFER-SAME: i64 [[TMP0:%.*]]) !continuation.registercount [[META2]] !continuation [[META6]] {
; REGISTERBUFFER-NEXT:  entryresume.0:
; REGISTERBUFFER-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 24)
; REGISTERBUFFER-NEXT:    [[MEM_RELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; REGISTERBUFFER-NEXT:    [[MEM_RELOAD:%.*]] = load ptr addrspace(1), ptr addrspace(32) [[MEM_RELOAD_ADDR]], align 4
; REGISTERBUFFER-NEXT:    [[ARG_RELOAD_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_ENTRY_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; REGISTERBUFFER-NEXT:    [[ARG_RELOAD:%.*]] = load <4 x i32>, ptr addrspace(32) [[ARG_RELOAD_ADDR]], align 4
; REGISTERBUFFER-NEXT:    store <4 x i32> [[ARG_RELOAD]], ptr addrspace(1) [[MEM_RELOAD]], align 4
; REGISTERBUFFER-NEXT:    call void @lgc.cps.free(i32 24)
; REGISTERBUFFER-NEXT:    ret void
; REGISTERBUFFER:       entryresume.0.split:
; REGISTERBUFFER-NEXT:    unreachable
;
