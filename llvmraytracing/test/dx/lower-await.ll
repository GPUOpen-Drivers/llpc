; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 3
; RUN: opt --verify-each -passes='lower-await,lint' -S %s --lint-abort-on-error | FileCheck -check-prefix=AWAIT %s
; RUN: opt --verify-each -passes='lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint' -S %s --lint-abort-on-error | FileCheck -check-prefix=CORO %s
; RUN: opt --verify-each -passes='lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,cleanup-continuations,lint' -S %s --lint-abort-on-error | FileCheck -check-prefix=CLEANED %s

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

declare ptr @async_fun(i32, i32)
declare ptr @async_fun_with_waitmask(i32, i32)
declare ptr @async_fun_with_arg(i32, i32, i32)
declare void @lgc.cps.await__void(...)
declare { i32 } @lgc.cps.await__i32(...)
declare void @lgc.cps.jump(...)
declare void @lgc.cps.complete()

define void @simple_await(i32 %dummyRetAddr) !continuation.registercount !1 {
; AWAIT-LABEL: define { ptr, ptr } @simple_await(
; AWAIT-SAME: i32 [[DUMMYRETADDR:%.*]], ptr [[TMP0:%.*]]) !continuation.registercount [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] {
; AWAIT-NEXT:    [[TMP2:%.*]] = call token @llvm.coro.id.retcon(i32 8, i32 4, ptr [[TMP0]], ptr @continuation.prototype.simple_await, ptr @continuation.malloc, ptr @continuation.free)
; AWAIT-NEXT:    [[TMP3:%.*]] = call ptr @llvm.coro.begin(token [[TMP2]], ptr null)
; AWAIT-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; AWAIT-NEXT:    [[TMP4:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; AWAIT-NEXT:    [[TMP5:%.*]] = call ptr [[TMP4]](i32 [[CALLEE]], i32 2), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; AWAIT-NEXT:    [[TMP6:%.*]] = call i1 (...) @llvm.coro.suspend.retcon.i1(ptr [[TMP5]])
; AWAIT-NEXT:    call void (...) @lgc.cps.jump(i32 [[DUMMYRETADDR]], i32 -1, i32 poison, i32 poison), !continuation.registercount [[META1]]
; AWAIT-NEXT:    unreachable
;
; CORO-LABEL: define { ptr, ptr } @simple_await(
; CORO-SAME: i32 [[DUMMYRETADDR:%.*]], ptr [[TMP0:%.*]]) !continuation.registercount [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] {
; CORO-NEXT:  AllocaSpillBB:
; CORO-NEXT:    [[DUMMYRETADDR_SPILL_ADDR:%.*]] = getelementptr inbounds [[SIMPLE_AWAIT_FRAME:%.*]], ptr [[TMP0]], i32 0, i32 0
; CORO-NEXT:    store i32 [[DUMMYRETADDR]], ptr [[DUMMYRETADDR_SPILL_ADDR]], align 4
; CORO-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; CORO-NEXT:    [[TMP1:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CORO-NEXT:    [[TMP2:%.*]] = call ptr [[TMP1]](i32 [[CALLEE]], i32 2), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; CORO-NEXT:    [[TMP3:%.*]] = insertvalue { ptr, ptr } poison, ptr @simple_await.resume.0, 0
; CORO-NEXT:    [[TMP4:%.*]] = insertvalue { ptr, ptr } [[TMP3]], ptr [[TMP2]], 1
; CORO-NEXT:    ret { ptr, ptr } [[TMP4]]
;
; CLEANED-LABEL: define void @simple_await(
; CLEANED-SAME: i32 [[CSPINIT:%.*]], i32 [[DUMMYRETADDR:%.*]]) !continuation.registercount [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] !continuation.stacksize [[META3:![0-9]+]] !continuation.state [[META3]] {
; CLEANED-NEXT:  AllocaSpillBB:
; CLEANED-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CLEANED-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], 8
; CLEANED-NEXT:    store i32 [[TMP5]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(21)
; CLEANED-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(21) [[TMP2]], i32 0
; CLEANED-NEXT:    store i32 [[DUMMYRETADDR]], ptr addrspace(21) [[TMP3]], align 4
; CLEANED-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; CLEANED-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CLEANED-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @simple_await.resume.0)
; CLEANED-NEXT:    [[TMP6:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    call void (...) @lgc.cps.jump(i32 [[CALLEE]], i32 -1, i32 [[TMP6]], i32 [[TMP1]]), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; CLEANED-NEXT:    unreachable
;
  %callee = ptrtoint ptr @async_fun to i32
  call void (...) @lgc.cps.await__void(i32 %callee, i32 2), !continuation.registercount !1, !continuation.returnedRegistercount !1
  call void (...) @lgc.cps.jump(i32 %dummyRetAddr, i32 -1, i32 poison, i32 poison), !continuation.registercount !1
  unreachable
}

define void @simple_await_entry() !continuation.entry !0 !continuation.registercount !1 {
; AWAIT-LABEL: define { ptr, ptr } @simple_await_entry(
; AWAIT-SAME: ptr [[TMP0:%.*]]) !continuation.registercount [[META1]] !continuation.entry [[META3:![0-9]+]] !continuation [[META4:![0-9]+]] {
; AWAIT-NEXT:    [[TMP2:%.*]] = call token @llvm.coro.id.retcon(i32 8, i32 4, ptr [[TMP0]], ptr @continuation.prototype.simple_await_entry, ptr @continuation.malloc, ptr @continuation.free)
; AWAIT-NEXT:    [[TMP3:%.*]] = call ptr @llvm.coro.begin(token [[TMP2]], ptr null)
; AWAIT-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; AWAIT-NEXT:    [[TMP4:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; AWAIT-NEXT:    [[TMP5:%.*]] = call ptr [[TMP4]](i32 [[CALLEE]], i32 2), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; AWAIT-NEXT:    [[TMP6:%.*]] = call i1 (...) @llvm.coro.suspend.retcon.i1(ptr [[TMP5]])
; AWAIT-NEXT:    call void @lgc.cps.complete()
; AWAIT-NEXT:    unreachable
;
; CORO-LABEL: define { ptr, ptr } @simple_await_entry(
; CORO-SAME: ptr [[TMP0:%.*]]) !continuation.registercount [[META1]] !continuation.entry [[META3:![0-9]+]] !continuation [[META4:![0-9]+]] {
; CORO-NEXT:  AllocaSpillBB:
; CORO-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; CORO-NEXT:    [[TMP1:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CORO-NEXT:    [[TMP2:%.*]] = call ptr [[TMP1]](i32 [[CALLEE]], i32 2), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; CORO-NEXT:    [[TMP3:%.*]] = insertvalue { ptr, ptr } poison, ptr @simple_await_entry.resume.0, 0
; CORO-NEXT:    [[TMP4:%.*]] = insertvalue { ptr, ptr } [[TMP3]], ptr [[TMP2]], 1
; CORO-NEXT:    ret { ptr, ptr } [[TMP4]]
;
; CLEANED-LABEL: define void @simple_await_entry(
; CLEANED-SAME: i32 [[CSPINIT:%.*]]) !continuation.registercount [[META1]] !continuation.entry [[META4:![0-9]+]] !continuation [[META5:![0-9]+]] !continuation.state [[META1]] {
; CLEANED-NEXT:  AllocaSpillBB:
; CLEANED-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CLEANED-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; CLEANED-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CLEANED-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @simple_await_entry.resume.0)
; CLEANED-NEXT:    [[TMP2:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    call void (...) @lgc.cps.jump(i32 [[CALLEE]], i32 -1, i32 [[TMP2]], i32 [[TMP1]]), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; CLEANED-NEXT:    unreachable
;
  %callee = ptrtoint ptr @async_fun to i32
  call void (...) @lgc.cps.await__void(i32 %callee, i32 2), !continuation.registercount !1, !continuation.returnedRegistercount !1
  ; Note: entry functions don't need a registercount annotation on return
  call void @lgc.cps.complete()
  unreachable
}

define void @await_with_arg(i32 %dummyRetAddr, i32 %i) !continuation.registercount !1 {
; AWAIT-LABEL: define { ptr, ptr } @await_with_arg(
; AWAIT-SAME: i32 [[DUMMYRETADDR:%.*]], i32 [[I:%.*]], ptr [[TMP0:%.*]]) !continuation.registercount [[META1]] !continuation [[META5:![0-9]+]] {
; AWAIT-NEXT:    [[TMP2:%.*]] = call token @llvm.coro.id.retcon(i32 8, i32 4, ptr [[TMP0]], ptr @continuation.prototype.await_with_arg, ptr @continuation.malloc, ptr @continuation.free)
; AWAIT-NEXT:    [[TMP3:%.*]] = call ptr @llvm.coro.begin(token [[TMP2]], ptr null)
; AWAIT-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun_with_arg to i32
; AWAIT-NEXT:    [[TMP4:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; AWAIT-NEXT:    [[TMP5:%.*]] = call ptr [[TMP4]](i32 [[CALLEE]], i32 2, i32 [[I]]), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; AWAIT-NEXT:    [[TMP6:%.*]] = call i1 (...) @llvm.coro.suspend.retcon.i1(ptr [[TMP5]])
; AWAIT-NEXT:    call void (...) @lgc.cps.jump(i32 [[DUMMYRETADDR]], i32 -1, i32 poison, i32 poison), !continuation.registercount [[META1]]
; AWAIT-NEXT:    unreachable
;
; CORO-LABEL: define { ptr, ptr } @await_with_arg(
; CORO-SAME: i32 [[DUMMYRETADDR:%.*]], i32 [[I:%.*]], ptr [[TMP0:%.*]]) !continuation.registercount [[META1]] !continuation [[META5:![0-9]+]] {
; CORO-NEXT:  AllocaSpillBB:
; CORO-NEXT:    [[DUMMYRETADDR_SPILL_ADDR:%.*]] = getelementptr inbounds [[AWAIT_WITH_ARG_FRAME:%.*]], ptr [[TMP0]], i32 0, i32 0
; CORO-NEXT:    store i32 [[DUMMYRETADDR]], ptr [[DUMMYRETADDR_SPILL_ADDR]], align 4
; CORO-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun_with_arg to i32
; CORO-NEXT:    [[TMP1:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CORO-NEXT:    [[TMP2:%.*]] = call ptr [[TMP1]](i32 [[CALLEE]], i32 2, i32 [[I]]), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; CORO-NEXT:    [[TMP3:%.*]] = insertvalue { ptr, ptr } poison, ptr @await_with_arg.resume.0, 0
; CORO-NEXT:    [[TMP4:%.*]] = insertvalue { ptr, ptr } [[TMP3]], ptr [[TMP2]], 1
; CORO-NEXT:    ret { ptr, ptr } [[TMP4]]
;
; CLEANED-LABEL: define void @await_with_arg(
; CLEANED-SAME: i32 [[CSPINIT:%.*]], i32 [[DUMMYRETADDR:%.*]], i32 [[I:%.*]]) !continuation.registercount [[META1]] !continuation [[META6:![0-9]+]] !continuation.stacksize [[META3]] !continuation.state [[META3]] {
; CLEANED-NEXT:  AllocaSpillBB:
; CLEANED-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CLEANED-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], 8
; CLEANED-NEXT:    store i32 [[TMP5]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(21)
; CLEANED-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(21) [[TMP2]], i32 0
; CLEANED-NEXT:    store i32 [[DUMMYRETADDR]], ptr addrspace(21) [[TMP3]], align 4
; CLEANED-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun_with_arg to i32
; CLEANED-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CLEANED-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @await_with_arg.resume.0)
; CLEANED-NEXT:    [[TMP6:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    call void (...) @lgc.cps.jump(i32 [[CALLEE]], i32 -1, i32 [[TMP6]], i32 [[TMP1]], i32 [[I]]), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; CLEANED-NEXT:    unreachable
;
  %callee = ptrtoint ptr @async_fun_with_arg to i32
  call void (...) @lgc.cps.await__void(i32 %callee, i32 2, i32 %i), !continuation.registercount !1,  !continuation.returnedRegistercount !1
  call void (...) @lgc.cps.jump(i32 %dummyRetAddr, i32 -1, i32 poison, i32 poison), !continuation.registercount !1
  unreachable
}

define i32 @await_with_ret_value(i32 %dummyRetAddr) !continuation.registercount !1 {
; AWAIT-LABEL: define { ptr, ptr } @await_with_ret_value(
; AWAIT-SAME: i32 [[DUMMYRETADDR:%.*]], ptr [[TMP0:%.*]]) !continuation.registercount [[META1]] !continuation [[META6:![0-9]+]] {
; AWAIT-NEXT:    [[TMP2:%.*]] = call token @llvm.coro.id.retcon(i32 8, i32 4, ptr [[TMP0]], ptr @continuation.prototype.await_with_ret_value, ptr @continuation.malloc, ptr @continuation.free)
; AWAIT-NEXT:    [[TMP3:%.*]] = call ptr @llvm.coro.begin(token [[TMP2]], ptr null)
; AWAIT-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; AWAIT-NEXT:    [[TMP4:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; AWAIT-NEXT:    [[TMP5:%.*]] = call ptr [[TMP4]](i32 [[CALLEE]], i32 2), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; AWAIT-NEXT:    [[TMP6:%.*]] = call i1 (...) @llvm.coro.suspend.retcon.i1(ptr [[TMP5]])
; AWAIT-NEXT:    [[TMP7:%.*]] = call { i32 } @lgc.ilcps.getReturnValue__sl_i32s()
; AWAIT-NEXT:    [[RES_2:%.*]] = extractvalue { i32 } [[TMP7]], 0
; AWAIT-NEXT:    call void (...) @lgc.cps.jump(i32 [[DUMMYRETADDR]], i32 -1, i32 poison, i32 poison, i32 [[RES_2]]), !continuation.registercount [[META1]]
; AWAIT-NEXT:    unreachable
;
; CORO-LABEL: define { ptr, ptr } @await_with_ret_value(
; CORO-SAME: i32 [[DUMMYRETADDR:%.*]], ptr [[TMP0:%.*]]) !continuation.registercount [[META1]] !continuation [[META6:![0-9]+]] {
; CORO-NEXT:  AllocaSpillBB:
; CORO-NEXT:    [[DUMMYRETADDR_SPILL_ADDR:%.*]] = getelementptr inbounds [[AWAIT_WITH_RET_VALUE_FRAME:%.*]], ptr [[TMP0]], i32 0, i32 0
; CORO-NEXT:    store i32 [[DUMMYRETADDR]], ptr [[DUMMYRETADDR_SPILL_ADDR]], align 4
; CORO-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; CORO-NEXT:    [[TMP1:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CORO-NEXT:    [[TMP2:%.*]] = call ptr [[TMP1]](i32 [[CALLEE]], i32 2), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; CORO-NEXT:    [[TMP3:%.*]] = insertvalue { ptr, ptr } poison, ptr @await_with_ret_value.resume.0, 0
; CORO-NEXT:    [[TMP4:%.*]] = insertvalue { ptr, ptr } [[TMP3]], ptr [[TMP2]], 1
; CORO-NEXT:    ret { ptr, ptr } [[TMP4]]
;
; CLEANED-LABEL: define void @await_with_ret_value(
; CLEANED-SAME: i32 [[CSPINIT:%.*]], i32 [[DUMMYRETADDR:%.*]]) !continuation.registercount [[META1]] !continuation [[META7:![0-9]+]] !continuation.stacksize [[META3]] !continuation.state [[META3]] {
; CLEANED-NEXT:  AllocaSpillBB:
; CLEANED-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CLEANED-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], 8
; CLEANED-NEXT:    store i32 [[TMP5]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(21)
; CLEANED-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(21) [[TMP2]], i32 0
; CLEANED-NEXT:    store i32 [[DUMMYRETADDR]], ptr addrspace(21) [[TMP3]], align 4
; CLEANED-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun to i32
; CLEANED-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CLEANED-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @await_with_ret_value.resume.0)
; CLEANED-NEXT:    [[TMP6:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    call void (...) @lgc.cps.jump(i32 [[CALLEE]], i32 -1, i32 [[TMP6]], i32 [[TMP1]]), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]]
; CLEANED-NEXT:    unreachable
;
  %callee = ptrtoint ptr @async_fun to i32
  %res = call { i32 } (...) @lgc.cps.await__i32(i32 %callee, i32 2), !continuation.registercount !1, !continuation.returnedRegistercount !1
  %res.2 = extractvalue { i32 } %res, 0
  call void (...) @lgc.cps.jump(i32 %dummyRetAddr, i32 -1, i32 poison, i32 poison, i32 %res.2), !continuation.registercount !1
  unreachable
}

define void @wait_await(i32 %dummyRetAddr) !continuation.registercount !1 {
; AWAIT-LABEL: define { ptr, ptr } @wait_await(
; AWAIT-SAME: i32 [[DUMMYRETADDR:%.*]], ptr [[TMP0:%.*]]) !continuation.registercount [[META1]] !continuation [[META7:![0-9]+]] {
; AWAIT-NEXT:    [[TMP2:%.*]] = call token @llvm.coro.id.retcon(i32 8, i32 4, ptr [[TMP0]], ptr @continuation.prototype.wait_await, ptr @continuation.malloc, ptr @continuation.free)
; AWAIT-NEXT:    [[TMP3:%.*]] = call ptr @llvm.coro.begin(token [[TMP2]], ptr null)
; AWAIT-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun_with_waitmask to i32
; AWAIT-NEXT:    [[TMP4:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; AWAIT-NEXT:    [[TMP5:%.*]] = call ptr [[TMP4]](i32 [[CALLEE]], i32 2), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]], !waitmask [[META8:![0-9]+]]
; AWAIT-NEXT:    [[TMP6:%.*]] = call i1 (...) @llvm.coro.suspend.retcon.i1(ptr [[TMP5]])
; AWAIT-NEXT:    call void (...) @lgc.cps.jump(i32 [[DUMMYRETADDR]], i32 -1, i32 poison, i32 poison, i32 poison), !continuation.registercount [[META1]]
; AWAIT-NEXT:    unreachable
;
; CORO-LABEL: define { ptr, ptr } @wait_await(
; CORO-SAME: i32 [[DUMMYRETADDR:%.*]], ptr [[TMP0:%.*]]) !continuation.registercount [[META1]] !continuation [[META7:![0-9]+]] {
; CORO-NEXT:  AllocaSpillBB:
; CORO-NEXT:    [[DUMMYRETADDR_SPILL_ADDR:%.*]] = getelementptr inbounds [[WAIT_AWAIT_FRAME:%.*]], ptr [[TMP0]], i32 0, i32 0
; CORO-NEXT:    store i32 [[DUMMYRETADDR]], ptr [[DUMMYRETADDR_SPILL_ADDR]], align 4
; CORO-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun_with_waitmask to i32
; CORO-NEXT:    [[TMP1:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CORO-NEXT:    [[TMP2:%.*]] = call ptr [[TMP1]](i32 [[CALLEE]], i32 2), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]], !waitmask [[META8:![0-9]+]]
; CORO-NEXT:    [[TMP3:%.*]] = insertvalue { ptr, ptr } poison, ptr @wait_await.resume.0, 0
; CORO-NEXT:    [[TMP4:%.*]] = insertvalue { ptr, ptr } [[TMP3]], ptr [[TMP2]], 1
; CORO-NEXT:    ret { ptr, ptr } [[TMP4]]
;
; CLEANED-LABEL: define void @wait_await(
; CLEANED-SAME: i32 [[CSPINIT:%.*]], i32 [[DUMMYRETADDR:%.*]]) !continuation.registercount [[META1]] !continuation [[META8:![0-9]+]] !continuation.stacksize [[META3]] !continuation.state [[META3]] {
; CLEANED-NEXT:  AllocaSpillBB:
; CLEANED-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CLEANED-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], 8
; CLEANED-NEXT:    store i32 [[TMP5]], ptr [[CSP]], align 4
; CLEANED-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(21)
; CLEANED-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(21) [[TMP2]], i32 0
; CLEANED-NEXT:    store i32 [[DUMMYRETADDR]], ptr addrspace(21) [[TMP3]], align 4
; CLEANED-NEXT:    [[CALLEE:%.*]] = ptrtoint ptr @async_fun_with_waitmask to i32
; CLEANED-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CALLEE]] to ptr
; CLEANED-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @wait_await.resume.0)
; CLEANED-NEXT:    [[TMP6:%.*]] = load i32, ptr [[CSP]], align 4
; CLEANED-NEXT:    call void (...) @lgc.cps.jump(i32 [[CALLEE]], i32 -1, i32 [[TMP6]], i32 [[TMP1]]), !continuation.registercount [[META1]], !continuation.returnedRegistercount [[META1]], !waitmask [[META9:![0-9]+]]
; CLEANED-NEXT:    unreachable
;
  %callee = ptrtoint ptr @async_fun_with_waitmask to i32
  call void (...) @lgc.cps.await__void(i32 %callee, i32 2), !waitmask !3, !continuation.registercount !1, !continuation.returnedRegistercount !1
  call void (...) @lgc.cps.jump(i32 %dummyRetAddr, i32 -1, i32 poison, i32 poison, i32 poison), !continuation.registercount !1
  unreachable
}

!continuation.stackAddrspace = !{!2}

!0 = !{}
!1 = !{i32 0}
!2 = !{i32 21}
!3 = !{i32 -1}
