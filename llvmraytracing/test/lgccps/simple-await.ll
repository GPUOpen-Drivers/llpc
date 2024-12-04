; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -S  -o - -passes='lower-await,coro-early,lgc-coro-split,coro-cleanup,cleanup-continuations' %s | FileCheck --check-prefixes=CHECK %s
; RUN: opt --verify-each -S  -o - -passes='lower-await' %s | FileCheck --check-prefixes=LOWER-AWAIT %s

!lgc.cps.module = !{}

declare !lgc.cps !0 void @callee({}, i32, float)

define void @test({} %state, i32 %rcr, float %arg) !lgc.cps !0 {
  %t0 = fadd float %arg, 1.0
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
  %t1 = call { float } (...) @lgc.cps.await__f32(i32 %cr, i32 2, float %t0), !continuation.returnedRegistercount !{i32 0}
  %res = extractvalue { float } %t1, 0
  %returnvalue = fmul float %res, %arg
  call void (...) @lgc.cps.jump(i32 %rcr, i32 2,  i32 poison, i32 poison, float %returnvalue)
  unreachable
}
!continuation.stackAddrspace = !{!1}

!0 = !{i32 1} ; level = 1
!1 = !{i32 5}

declare i32 @lgc.cps.as.continuation.reference(...) memory(none)
declare { float } @lgc.cps.await__f32(...)
declare void @lgc.cps.jump(...)
; CHECK-LABEL: define void @test(
; CHECK-SAME: i32 [[CSPINIT:%.*]], {} [[STATE:%.*]], i32 [[RCR:%.*]], float [[ARG:%.*]]) !lgc.cps [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] !continuation.stacksize [[META3:![0-9]+]] !continuation.state [[META3]] {
; CHECK-NEXT:  AllocaSpillBB:
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP3:%.*]] = add i32 [[TMP2]], 8
; CHECK-NEXT:    store i32 [[TMP3]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP2]], 4
; CHECK-NEXT:    [[TMP5:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP6:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP5]], i32 0
; CHECK-NEXT:    store float [[ARG]], ptr addrspace(5) [[TMP6]], align 4
; CHECK-NEXT:    [[TMP7:%.*]] = inttoptr i32 [[TMP2]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP8:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP7]], i32 0
; CHECK-NEXT:    store i32 [[RCR]], ptr addrspace(5) [[TMP8]], align 4
; CHECK-NEXT:    [[T0:%.*]] = fadd float [[ARG]], 1.000000e+00
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
; CHECK-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CR]] to ptr
; CHECK-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @test.resume.0)
; CHECK-NEXT:    [[TMP9:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP9]], i32 [[TMP1]], float [[T0]]), !continuation.returnedRegistercount [[META4:![0-9]+]]
; CHECK-NEXT:    unreachable
;
;
; CHECK-LABEL: define dso_local void @test.resume.0(
; CHECK-SAME: i32 [[CSPINIT:%.*]], i32 [[TMP0:%.*]], i32 [[TMP1:%.*]], float [[TMP2:%.*]]) !lgc.cps [[META1]] !continuation [[META2]] !continuation.registercount [[META4]] {
; CHECK-NEXT:  entryresume.0:
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP3:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP3]], -8
; CHECK-NEXT:    [[TMP13:%.*]] = insertvalue { float } poison, float [[TMP2]], 0
; CHECK-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], 4
; CHECK-NEXT:    [[TMP6:%.*]] = inttoptr i32 [[TMP5]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP7:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP6]], i32 0
; CHECK-NEXT:    [[ARG_RELOAD:%.*]] = load float, ptr addrspace(5) [[TMP7]], align 4
; CHECK-NEXT:    [[TMP8:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP8]], i32 0
; CHECK-NEXT:    [[RCR_RELOAD:%.*]] = load i32, ptr addrspace(5) [[TMP9]], align 4
; CHECK-NEXT:    [[RES1:%.*]] = extractvalue { float } [[TMP13]], 0
; CHECK-NEXT:    [[RETURNVALUE:%.*]] = fmul float [[RES1]], [[ARG_RELOAD]]
; CHECK-NEXT:    [[TMP10:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP11:%.*]] = add i32 [[TMP10]], -8
; CHECK-NEXT:    store i32 [[TMP11]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP12:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[RCR_RELOAD]], i32 2, i32 [[TMP12]], i32 poison, float [[RETURNVALUE]])
; CHECK-NEXT:    unreachable
;
;
; LOWER-AWAIT-LABEL: define { ptr, ptr } @test(
; LOWER-AWAIT-SAME: {} [[STATE:%.*]], i32 [[RCR:%.*]], float [[ARG:%.*]], ptr [[TMP0:%.*]]) !lgc.cps [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] {
; LOWER-AWAIT-NEXT:    [[TMP2:%.*]] = call token @llvm.coro.id.retcon(i32 8, i32 4, ptr [[TMP0]], ptr @continuation.prototype.test, ptr @continuation.malloc, ptr @continuation.free)
; LOWER-AWAIT-NEXT:    [[TMP3:%.*]] = call ptr @llvm.coro.begin(token [[TMP2]], ptr null)
; LOWER-AWAIT-NEXT:    [[T0:%.*]] = fadd float [[ARG]], 1.000000e+00
; LOWER-AWAIT-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
; LOWER-AWAIT-NEXT:    [[TMP4:%.*]] = inttoptr i32 [[CR]] to ptr
; LOWER-AWAIT-NEXT:    [[TMP5:%.*]] = call ptr [[TMP4]](i32 [[CR]], i32 2, float [[T0]]), !continuation.returnedRegistercount [[META3:![0-9]+]]
; LOWER-AWAIT-NEXT:    [[TMP6:%.*]] = call i1 (...) @llvm.coro.suspend.retcon.i1(ptr [[TMP5]])
; LOWER-AWAIT-NEXT:    [[TMP8:%.*]] = call { float } @lgc.ilcps.getReturnValue__sl_f32s()
; LOWER-AWAIT-NEXT:    [[TMP7:%.*]] = extractvalue { float } [[TMP8]], 0
; LOWER-AWAIT-NEXT:    [[RETURNVALUE:%.*]] = fmul float [[TMP7]], [[ARG]]
; LOWER-AWAIT-NEXT:    call void (...) @lgc.cps.jump(i32 [[RCR]], i32 2, i32 poison, i32 poison, float [[RETURNVALUE]])
; LOWER-AWAIT-NEXT:    unreachable
;
