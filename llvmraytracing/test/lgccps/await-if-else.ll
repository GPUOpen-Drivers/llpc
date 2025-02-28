; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -S -o - -passes='lower-await,coro-early,lgc-coro-split,coro-cleanup,cleanup-continuations' %s | FileCheck --check-prefixes=CHECK %s
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

declare !lgc.cps !0 void @callee({}, i32, float)
declare !lgc.cps !0 void @callee2({}, i32, float)

define void @test(i32 %shaderIndex, i32 %rcr, float %arg) !lgc.cps !0 {
  %t0 = fadd float %arg, 1.0
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
  %cr2 = call i32 @lgc.cps.as.continuation.reference(ptr @callee2)
  %cond = fcmp olt float %t0, 1.0
  br i1 %cond, label %bb1, label %bb2

bb1:
  %t1 = call { i32, float } (...) @lgc.cps.await__sl_i32f32(i32 %cr, i32 2, i32 poison, float %arg), !continuation.returnedRegistercount !{i32 0}
  %res.1 = extractvalue { i32, float } %t1, 1
  br label %bb3

bb2:
  %t2 = call { i32, float } (...) @lgc.cps.await__sl_i32f32(i32 %cr2, i32 2, i32 poison, float %t0), !continuation.returnedRegistercount !{i32 0}
  %res.2 = extractvalue { i32, float } %t2, 1
  br label %bb3

bb3:
  %t3 = phi float [%res.1, %bb1], [%res.2, %bb2]
  %returnvalue = fmul float %t3, %arg
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
; CHECK-SAME: i32 [[CSPINIT:%.*]], i32 [[SHADERINDEX:%.*]], i32 [[RCR:%.*]], float [[ARG:%.*]]) !lgc.cps [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] !continuation.stacksize [[META3:![0-9]+]] !continuation.state [[META3]] {
; CHECK-NEXT:  AllocaSpillBB:
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP13:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP14:%.*]] = add i32 [[TMP13]], 8
; CHECK-NEXT:    store i32 [[TMP14]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP13]], 4
; CHECK-NEXT:    [[TMP5:%.*]] = inttoptr i32 [[TMP13]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP6:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP5]], i32 0
; CHECK-NEXT:    store i32 [[RCR]], ptr addrspace(5) [[TMP6]], align 4
; CHECK-NEXT:    [[TMP7:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP8:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP7]], i32 0
; CHECK-NEXT:    store float [[ARG]], ptr addrspace(5) [[TMP8]], align 4
; CHECK-NEXT:    [[T0:%.*]] = fadd float [[ARG]], 1.000000e+00
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @callee)
; CHECK-NEXT:    [[CR2:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @callee2)
; CHECK-NEXT:    [[COND:%.*]] = fcmp olt float [[T0]], 1.000000e+00
; CHECK-NEXT:    br i1 [[COND]], label [[BB1:%.*]], label [[BB2:%.*]]
; CHECK:       bb1:
; CHECK-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[CR]] to ptr
; CHECK-NEXT:    [[TMP1:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @test.resume.0)
; CHECK-NEXT:    [[TMP9:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP9]], i32 poison, i32 [[TMP1]], float [[ARG]]), !continuation.returnedRegistercount [[META4:![0-9]+]]
; CHECK-NEXT:    unreachable
; CHECK:       bb2:
; CHECK-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[CR2]] to ptr
; CHECK-NEXT:    [[TMP3:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @test.resume.1)
; CHECK-NEXT:    [[TMP12:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR2]], i32 2, i32 [[TMP12]], i32 poison, i32 [[TMP3]], float [[T0]]), !continuation.returnedRegistercount [[META4]]
; CHECK-NEXT:    unreachable
;
;
; CHECK-LABEL: define dso_local void @test.resume.0(
; CHECK-SAME: i32 [[CSPINIT:%.*]], i32 [[TMP0:%.*]], float [[TMP1:%.*]]) !lgc.cps [[META1]] !continuation [[META2]] !continuation.registercount [[META4]] {
; CHECK-NEXT:  entryresume.0:
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP3:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP3]], -8
; CHECK-NEXT:    [[TMP13:%.*]] = insertvalue { i32, float } poison, i32 [[TMP0]], 0
; CHECK-NEXT:    [[TMP14:%.*]] = insertvalue { i32, float } [[TMP13]], float [[TMP1]], 1
; CHECK-NEXT:    [[RES_11:%.*]] = extractvalue { i32, float } [[TMP14]], 1
; CHECK-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], 4
; CHECK-NEXT:    [[TMP6:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP7:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP6]], i32 0
; CHECK-NEXT:    [[RCR_RELOAD:%.*]] = load i32, ptr addrspace(5) [[TMP7]], align 4
; CHECK-NEXT:    [[TMP8:%.*]] = inttoptr i32 [[TMP5]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP8]], i32 0
; CHECK-NEXT:    [[ARG_RELOAD:%.*]] = load float, ptr addrspace(5) [[TMP9]], align 4
; CHECK-NEXT:    [[RETURNVALUE:%.*]] = fmul float [[RES_11]], [[ARG_RELOAD]]
; CHECK-NEXT:    [[TMP10:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP11:%.*]] = add i32 [[TMP10]], -8
; CHECK-NEXT:    store i32 [[TMP11]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP12:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[RCR_RELOAD]], i32 2, i32 [[TMP12]], i32 poison, i32 poison, float [[RETURNVALUE]])
; CHECK-NEXT:    unreachable
;
;
; CHECK-LABEL: define dso_local void @test.resume.1(
; CHECK-SAME: i32 [[CSPINIT:%.*]], i32 [[TMP0:%.*]], float [[TMP1:%.*]]) !lgc.cps [[META1]] !continuation [[META2]] !continuation.registercount [[META4]] {
; CHECK-NEXT:  entryresume.1:
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP3:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP3]], -8
; CHECK-NEXT:    [[TMP13:%.*]] = insertvalue { i32, float } poison, i32 [[TMP0]], 0
; CHECK-NEXT:    [[TMP14:%.*]] = insertvalue { i32, float } [[TMP13]], float [[TMP1]], 1
; CHECK-NEXT:    [[RES_21:%.*]] = extractvalue { i32, float } [[TMP14]], 1
; CHECK-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], 4
; CHECK-NEXT:    [[TMP6:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP7:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP6]], i32 0
; CHECK-NEXT:    [[RCR_RELOAD:%.*]] = load i32, ptr addrspace(5) [[TMP7]], align 4
; CHECK-NEXT:    [[TMP8:%.*]] = inttoptr i32 [[TMP5]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP8]], i32 0
; CHECK-NEXT:    [[ARG_RELOAD:%.*]] = load float, ptr addrspace(5) [[TMP9]], align 4
; CHECK-NEXT:    [[RETURNVALUE:%.*]] = fmul float [[RES_21]], [[ARG_RELOAD]]
; CHECK-NEXT:    [[TMP10:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP11:%.*]] = add i32 [[TMP10]], -8
; CHECK-NEXT:    store i32 [[TMP11]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP12:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[RCR_RELOAD]], i32 2, i32 [[TMP12]], i32 poison, i32 poison, float [[RETURNVALUE]])
; CHECK-NEXT:    unreachable
;
