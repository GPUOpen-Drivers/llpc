; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -S -o - -passes='lower-await,coro-early,lgc-coro-split,coro-cleanup,cleanup-continuations' %s | FileCheck --check-prefixes=CHECK %s

declare !lgc.cps !0 void @callee({}, i32, i32)

define void @test(i32 %shaderIndex, i32 %rcr, float %arg, float %arg2) !lgc.cps !0 {
entry:
  %t0 = fadd float %arg, 1.0
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
  br label %loop

loop:
  %ind = phi i32 [0, %entry], [%inc, %loop]
  %t1 = call { i32, float } (...) @lgc.cps.await__sl_i32f32(i32 %cr, i32 2, i32 poison, i32 %ind), !continuation.returnedRegistercount !{i32 0}
  %inc = add i32 %ind, 1
  %res = extractvalue { i32, float } %t1, 1
  %cond = fcmp olt float %res, 5.0
  br i1 %cond, label %loop, label %end

end:
  %t2 = fmul float %res, %arg
  %returnvalue = fadd float %t2, %arg2
  call void (...) @lgc.cps.jump(i32 %rcr, i32 2,  i32 poison, i32 poison, i32 poison, float %returnvalue)
  unreachable
}

!continuation.stackAddrspace = !{!1}

!0 = !{i32 1} ; level = 1
!1 = !{i32 5}

declare i32 @lgc.cps.as.continuation.reference(...) memory(none)
declare { i32, float } @lgc.cps.await__sl_i32f32(...)
declare void @lgc.cps.jump(...)
; CHECK-LABEL: define void @test(
; CHECK-SAME: i32 [[CSPINIT:%.*]], i32 [[SHADERINDEX:%.*]], i32 [[RCR:%.*]], float [[ARG:%.*]], float [[ARG2:%.*]]) !lgc.cps [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] !continuation.stacksize [[META3:![0-9]+]] !continuation.state [[META3]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP3:%.*]] = add i32 [[TMP2]], 20
; CHECK-NEXT:    store i32 [[TMP3]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP7:%.*]] = add i32 [[TMP2]], 4
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP2]], 8
; CHECK-NEXT:    [[TMP12:%.*]] = add i32 [[TMP2]], 12
; CHECK-NEXT:    [[TMP5:%.*]] = inttoptr i32 [[TMP2]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP6:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP5]], i32 0
; CHECK-NEXT:    store i32 [[RCR]], ptr addrspace(5) [[TMP6]], align 4
; CHECK-NEXT:    [[TMP8:%.*]] = inttoptr i32 [[TMP7]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP8]], i32 0
; CHECK-NEXT:    store float [[ARG]], ptr addrspace(5) [[TMP9]], align 4
; CHECK-NEXT:    [[TMP10:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP11:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP10]], i32 0
; CHECK-NEXT:    store float [[ARG2]], ptr addrspace(5) [[TMP11]], align 4
; CHECK-NEXT:    [[T0:%.*]] = fadd float [[ARG]], 1.000000e+00
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
; CHECK-NEXT:    [[TMP13:%.*]] = inttoptr i32 [[TMP12]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP14:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP13]], i32 0
; CHECK-NEXT:    store i32 [[CR]], ptr addrspace(5) [[TMP14]], align 4
; CHECK-NEXT:    [[TMP15:%.*]] = add i32 [[TMP2]], 16
; CHECK-NEXT:    [[TMP16:%.*]] = inttoptr i32 [[TMP15]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP17:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP16]], i32 0
; CHECK-NEXT:    store i32 0, ptr addrspace(5) [[TMP17]], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CR]] to ptr
; CHECK-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @test.resume.0)
; CHECK-NEXT:    [[TMP18:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP18]], i32 poison, i32 [[TMP1]], i32 0), !continuation.returnedRegistercount [[META4:![0-9]+]]
; CHECK-NEXT:    unreachable
;
;
; CHECK-LABEL: define dso_local void @test.resume.0(
; CHECK-SAME: i32 [[CSPINIT:%.*]], i32 [[TMP0:%.*]], float [[TMP1:%.*]]) !lgc.cps [[META1]] !continuation [[META2]] !continuation.registercount [[META4]] {
; CHECK-NEXT:  entryresume.0:
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP18:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP7:%.*]] = add i32 [[TMP18]], -20
; CHECK-NEXT:    [[TMP4:%.*]] = insertvalue { i32, float } poison, i32 [[TMP0]], 0
; CHECK-NEXT:    [[TMP31:%.*]] = insertvalue { i32, float } [[TMP4]], float [[TMP1]], 1
; CHECK-NEXT:    [[TMP8:%.*]] = add i32 [[TMP7]], 16
; CHECK-NEXT:    [[TMP9:%.*]] = inttoptr i32 [[TMP8]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP10:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP9]], i32 0
; CHECK-NEXT:    [[IND_RELOAD:%.*]] = load i32, ptr addrspace(5) [[TMP10]], align 4
; CHECK-NEXT:    [[INC:%.*]] = add i32 [[IND_RELOAD]], 1
; CHECK-NEXT:    [[RES1:%.*]] = extractvalue { i32, float } [[TMP31]], 1
; CHECK-NEXT:    [[COND:%.*]] = fcmp olt float [[RES1]], 5.000000e+00
; CHECK-NEXT:    br i1 [[COND]], label [[LOOP_FROM_AFTERCOROSUSPEND:%.*]], label [[END:%.*]]
; CHECK:       loop.from.AfterCoroSuspend:
; CHECK-NEXT:    [[INC_LOOP:%.*]] = phi i32 [ [[INC]], [[ENTRYRESUME_0:%.*]] ]
; CHECK-NEXT:    [[TMP14:%.*]] = add i32 [[TMP7]], 12
; CHECK-NEXT:    [[TMP11:%.*]] = add i32 [[TMP7]], 16
; CHECK-NEXT:    [[TMP12:%.*]] = inttoptr i32 [[TMP11]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP13:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP12]], i32 0
; CHECK-NEXT:    store i32 [[INC_LOOP]], ptr addrspace(5) [[TMP13]], align 4
; CHECK-NEXT:    [[TMP15:%.*]] = inttoptr i32 [[TMP14]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP16:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP15]], i32 0
; CHECK-NEXT:    [[CR_RELOAD:%.*]] = load i32, ptr addrspace(5) [[TMP16]], align 4
; CHECK-NEXT:    [[TMP5:%.*]] = inttoptr i32 [[CR_RELOAD]] to ptr
; CHECK-NEXT:    [[TMP6:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @test.resume.0)
; CHECK-NEXT:    [[TMP28:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR_RELOAD]], i32 2, i32 [[TMP28]], i32 poison, i32 [[TMP6]], i32 [[INC_LOOP]]), !continuation.returnedRegistercount [[META4]]
; CHECK-NEXT:    unreachable
; CHECK:       end:
; CHECK-NEXT:    [[TMP20:%.*]] = add i32 [[TMP7]], 4
; CHECK-NEXT:    [[TMP17:%.*]] = add i32 [[TMP7]], 8
; CHECK-NEXT:    [[TMP29:%.*]] = inttoptr i32 [[TMP7]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP19:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP29]], i32 0
; CHECK-NEXT:    [[RCR_RELOAD:%.*]] = load i32, ptr addrspace(5) [[TMP19]], align 4
; CHECK-NEXT:    [[TMP21:%.*]] = inttoptr i32 [[TMP20]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP22:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP21]], i32 0
; CHECK-NEXT:    [[ARG_RELOAD:%.*]] = load float, ptr addrspace(5) [[TMP22]], align 4
; CHECK-NEXT:    [[TMP23:%.*]] = inttoptr i32 [[TMP17]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP24:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP23]], i32 0
; CHECK-NEXT:    [[ARG2_RELOAD:%.*]] = load float, ptr addrspace(5) [[TMP24]], align 4
; CHECK-NEXT:    [[T2:%.*]] = fmul float [[RES1]], [[ARG_RELOAD]]
; CHECK-NEXT:    [[RETURNVALUE:%.*]] = fadd float [[T2]], [[ARG2_RELOAD]]
; CHECK-NEXT:    [[TMP25:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP26:%.*]] = add i32 [[TMP25]], -20
; CHECK-NEXT:    store i32 [[TMP26]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP27:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[RCR_RELOAD]], i32 2, i32 [[TMP27]], i32 poison, i32 poison, float [[RETURNVALUE]])
; CHECK-NEXT:    unreachable
;
