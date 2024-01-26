; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 2
; RUN: opt --verify-each -S  -o - -passes='lower-await,coro-early,lgc-coro-split,coro-cleanup,cleanup-continuations' %s | FileCheck --check-prefixes=CHECK %s
declare !lgc.cps !0 void @callee({}, i32, float)

define void @test({} %state, i32 %rcr, float %arg, i32 %arg1) !lgc.cps !0 {
  %a1 = alloca i32
  %a2 = alloca i32
  %cond = icmp ult i32 %arg1, 0
  %p = select i1 %cond, ptr %a1, ptr %a2
  store i32 111, ptr %p, align 4
  %t0 = fadd float %arg, 1.0
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
  %t1 = call float (...) @lgc.cps.await.f32(i32 %cr, i32 2, float %t0)
  %tmp = fmul float %t1, %arg
  %v111 = load float, ptr %p, align 4
  %returnvalue = fmul float %tmp, %v111
  call void (...) @lgc.cps.jump(i32 %rcr, i32 2, {} poison, i32 poison, float %returnvalue)
  unreachable
}

!0 = !{i32 1} ; level = 1

declare i32 @lgc.cps.as.continuation.reference(...) memory(none)
declare float @lgc.cps.await.f32(...)
declare void @lgc.cps.jump(...)
; CHECK-LABEL: define void @test
; CHECK-SAME: ({} [[STATE:%.*]], i32 [[RCR:%.*]], float [[ARG:%.*]], i32 [[ARG1:%.*]]) !lgc.cps [[META0:![0-9]+]] !continuation [[META1:![0-9]+]] {
; CHECK-NEXT:  AllocaSpillBB:
; CHECK-NEXT:    [[TMP0:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 20)
; CHECK-NEXT:    [[A1:%.*]] = getelementptr inbounds [[TEST_FRAME:%.*]], ptr addrspace(32) [[TMP0]], i32 0, i32 0
; CHECK-NEXT:    [[A2:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP0]], i32 0, i32 1
; CHECK-NEXT:    [[ARG1_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP0]], i32 0, i32 4
; CHECK-NEXT:    store i32 [[ARG1]], ptr addrspace(32) [[ARG1_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[ARG_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP0]], i32 0, i32 3
; CHECK-NEXT:    store float [[ARG]], ptr addrspace(32) [[ARG_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[RCR_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP0]], i32 0, i32 2
; CHECK-NEXT:    store i32 [[RCR]], ptr addrspace(32) [[RCR_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[COND:%.*]] = icmp ult i32 [[ARG1]], 0
; CHECK-NEXT:    [[P:%.*]] = select i1 [[COND]], ptr addrspace(32) [[A1]], ptr addrspace(32) [[A2]]
; CHECK-NEXT:    store i32 111, ptr addrspace(32) [[P]], align 4
; CHECK-NEXT:    [[T0:%.*]] = fadd float [[ARG]], 1.000000e+00
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
; CHECK-NEXT:    [[TMP1:%.*]] = inttoptr i32 [[CR]] to ptr
; CHECK-NEXT:    [[TMP2:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @test.resume.0)
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, {} poison, i32 [[TMP2]], float [[T0]])
; CHECK-NEXT:    unreachable
;
;
; CHECK-LABEL: define dso_local void @test.resume.0
; CHECK-SAME: ({} [[TMP0:%.*]], i32 [[TMP1:%.*]], i32 [[TMP2:%.*]], float [[TMP3:%.*]]) !lgc.cps [[META0]] !continuation [[META1]] {
; CHECK-NEXT:  entryresume.0:
; CHECK-NEXT:    [[TMP4:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 20)
; CHECK-NEXT:    [[A1:%.*]] = getelementptr inbounds [[TEST_FRAME:%.*]], ptr addrspace(32) [[TMP4]], i32 0, i32 0
; CHECK-NEXT:    [[A2:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 1
; CHECK-NEXT:    [[ARG1_RELOAD_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 4
; CHECK-NEXT:    [[ARG1_RELOAD:%.*]] = load i32, ptr addrspace(32) [[ARG1_RELOAD_ADDR]], align 4
; CHECK-NEXT:    [[ARG_RELOAD_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 3
; CHECK-NEXT:    [[ARG_RELOAD:%.*]] = load float, ptr addrspace(32) [[ARG_RELOAD_ADDR]], align 4
; CHECK-NEXT:    [[RCR_RELOAD_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 2
; CHECK-NEXT:    [[RCR_RELOAD:%.*]] = load i32, ptr addrspace(32) [[RCR_RELOAD_ADDR]], align 4
; CHECK-NEXT:    [[COND2:%.*]] = icmp ult i32 [[ARG1_RELOAD]], 0
; CHECK-NEXT:    [[P1:%.*]] = select i1 [[COND2]], ptr addrspace(32) [[A1]], ptr addrspace(32) [[A2]]
; CHECK-NEXT:    [[TMP:%.*]] = fmul float [[TMP3]], [[ARG_RELOAD]]
; CHECK-NEXT:    [[V111:%.*]] = load float, ptr addrspace(32) [[P1]], align 4
; CHECK-NEXT:    [[RETURNVALUE:%.*]] = fmul float [[TMP]], [[V111]]
; CHECK-NEXT:    call void @lgc.cps.free(i32 20)
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[RCR_RELOAD]], i32 2, {} poison, i32 poison, float [[RETURNVALUE]])
; CHECK-NEXT:    unreachable
;
