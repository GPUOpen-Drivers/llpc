; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -S -o - -passes='lower-await,coro-early,lgc-coro-split,coro-cleanup,cleanup-continuations' %s | FileCheck --check-prefixes=CHECK %s

!lgc.cps.module = !{}

declare !lgc.cps !0 void @callee({}, i32, i32)

define void @test({} %state, i32 %rcr, float %arg, float %arg2) !lgc.cps !0 {
entry:
  %t0 = fadd float %arg, 1.0
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
  br label %loop

loop:
  %ind = phi i32 [0, %entry], [%inc, %loop]
  %t1 = call float (...) @lgc.cps.await__f32(i32 %cr, i32 2, i32 %ind), !continuation.returnedRegistercount !{i32 0}
  %inc = add i32 %ind, 1
  %cond = fcmp olt float %t1, 5.0
  br i1 %cond, label %loop, label %end

end:
  %t2 = fmul float %t1, %arg
  %returnvalue = fadd float %t2, %arg2
  call void (...) @lgc.cps.jump(i32 %rcr, i32 2, {} poison, i32 poison, i32 poison, float %returnvalue)
  unreachable
}

!0 = !{i32 1} ; level = 1

declare i32 @lgc.cps.as.continuation.reference(...) memory(none)
declare float @lgc.cps.await__f32(...)
declare void @lgc.cps.jump(...)
; CHECK-LABEL: define void @test(
; CHECK-SAME: {} [[STATE:%.*]], i32 [[RCR:%.*]], float [[ARG:%.*]], float [[ARG2:%.*]]) !lgc.cps [[META0:![0-9]+]] !continuation [[META1:![0-9]+]] !continuation.stacksize [[META2:![0-9]+]] !continuation.state [[META2]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[CONT_STATE_STACK_SEGMENT:%.*]] = call ptr addrspace(32) @lgc.cps.alloc(i32 20)
; CHECK-NEXT:    [[ARG2_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME:%.*]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 2
; CHECK-NEXT:    store float [[ARG2]], ptr addrspace(32) [[ARG2_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[ARG_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 1
; CHECK-NEXT:    store float [[ARG]], ptr addrspace(32) [[ARG_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[RCR_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 0
; CHECK-NEXT:    store i32 [[RCR]], ptr addrspace(32) [[RCR_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[T0:%.*]] = fadd float [[ARG]], 1.000000e+00
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
; CHECK-NEXT:    [[CR_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 3
; CHECK-NEXT:    store i32 [[CR]], ptr addrspace(32) [[CR_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[IND_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[CONT_STATE_STACK_SEGMENT]], i32 0, i32 4
; CHECK-NEXT:    store i32 0, ptr addrspace(32) [[IND_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CR]] to ptr
; CHECK-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference__i32(ptr @test.resume.0)
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, {} poison, i32 poison, i32 [[TMP1]], i32 0), !continuation.returnedRegistercount [[META3:![0-9]+]]
; CHECK-NEXT:    unreachable
;
;
; CHECK-LABEL: define dso_local void @test.resume.0(
; CHECK-SAME: {} [[TMP0:%.*]], i32 [[TMP1:%.*]], i32 [[TMP2:%.*]], float [[TMP3:%.*]]) !lgc.cps [[META0]] !continuation [[META1]] !continuation.registercount [[META3]] {
; CHECK-NEXT:  entryresume.0:
; CHECK-NEXT:    [[TMP4:%.*]] = call ptr addrspace(32) @lgc.cps.peek(i32 20)
; CHECK-NEXT:    [[IND_RELOAD_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME:%.*]], ptr addrspace(32) [[TMP4]], i32 0, i32 4
; CHECK-NEXT:    [[IND_RELOAD:%.*]] = load i32, ptr addrspace(32) [[IND_RELOAD_ADDR]], align 4
; CHECK-NEXT:    [[INC:%.*]] = add i32 [[IND_RELOAD]], 1
; CHECK-NEXT:    [[COND:%.*]] = fcmp olt float [[TMP3]], 5.000000e+00
; CHECK-NEXT:    br i1 [[COND]], label [[LOOP_FROM_AFTERCOROSUSPEND:%.*]], label [[END:%.*]]
; CHECK:       loop.from.AfterCoroSuspend:
; CHECK-NEXT:    [[INC_LOOP:%.*]] = phi i32 [ [[INC]], [[ENTRYRESUME_0:%.*]] ]
; CHECK-NEXT:    [[IND_SPILL_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 4
; CHECK-NEXT:    store i32 [[INC_LOOP]], ptr addrspace(32) [[IND_SPILL_ADDR]], align 4
; CHECK-NEXT:    [[CR_RELOAD_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 3
; CHECK-NEXT:    [[CR_RELOAD:%.*]] = load i32, ptr addrspace(32) [[CR_RELOAD_ADDR]], align 4
; CHECK-NEXT:    [[TMP5:%.*]] = inttoptr i32 [[CR_RELOAD]] to ptr
; CHECK-NEXT:    [[TMP6:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference__i32(ptr @test.resume.0)
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR_RELOAD]], i32 2, {} poison, i32 poison, i32 [[TMP6]], i32 [[INC_LOOP]]), !continuation.returnedRegistercount [[META3]]
; CHECK-NEXT:    unreachable
; CHECK:       end:
; CHECK-NEXT:    [[ARG2_RELOAD_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 2
; CHECK-NEXT:    [[ARG2_RELOAD:%.*]] = load float, ptr addrspace(32) [[ARG2_RELOAD_ADDR]], align 4
; CHECK-NEXT:    [[ARG_RELOAD_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 1
; CHECK-NEXT:    [[ARG_RELOAD:%.*]] = load float, ptr addrspace(32) [[ARG_RELOAD_ADDR]], align 4
; CHECK-NEXT:    [[RCR_RELOAD_ADDR:%.*]] = getelementptr inbounds [[TEST_FRAME]], ptr addrspace(32) [[TMP4]], i32 0, i32 0
; CHECK-NEXT:    [[RCR_RELOAD:%.*]] = load i32, ptr addrspace(32) [[RCR_RELOAD_ADDR]], align 4
; CHECK-NEXT:    [[T2:%.*]] = fmul float [[TMP3]], [[ARG_RELOAD]]
; CHECK-NEXT:    [[RETURNVALUE:%.*]] = fadd float [[T2]], [[ARG2_RELOAD]]
; CHECK-NEXT:    call void @lgc.cps.free(i32 20)
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[RCR_RELOAD]], i32 2, {} poison, i32 poison, i32 poison, float [[RETURNVALUE]])
; CHECK-NEXT:    unreachable
;
