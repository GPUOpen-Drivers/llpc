; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 5
; RUN: opt -o - -passes='lower-await,coro-early,lgc-coro-split,coro-cleanup,cleanup-continuations' %s -S | FileCheck --check-prefixes=CHECK %s
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

declare void @lgc.cps.jump(...) noreturn

define void @unify_jumps(i32 %arg, ptr %table) !lgc.cps !0 !lgc.shaderstage !{i32 7} {
; CHECK-LABEL: define void @unify_jumps(
; CHECK-SAME: i32 [[ARG:%.*]], ptr [[TABLE:%.*]]) !lgc.cps [[META1:![0-9]+]] !lgc.shaderstage [[META2:![0-9]+]] {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[COND:%.*]] = icmp ult i32 [[ARG]], 3
; CHECK-NEXT:    br i1 [[COND]], label %[[THEN:.*]], label %[[ELSE:.*]]
; CHECK:       [[THEN]]:
; CHECK-NEXT:    [[TABLE_0:%.*]] = getelementptr i32, ptr [[TABLE]], i32 0
; CHECK-NEXT:    [[CR_THEN:%.*]] = load i32, ptr [[TABLE_0]], align 4
; CHECK-NEXT:    [[THEN_ARG:%.*]] = add i32 [[ARG]], 1
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR_THEN]], i32 2, i32 poison, i32 poison, i32 [[THEN_ARG]])
; CHECK-NEXT:    unreachable
; CHECK:       [[ELSE]]:
; CHECK-NEXT:    [[TABLE_1:%.*]] = getelementptr i32, ptr [[TABLE]], i32 1
; CHECK-NEXT:    [[CR_ELSE:%.*]] = load i32, ptr [[TABLE_1]], align 4
; CHECK-NEXT:    [[ELSE_ARG:%.*]] = uitofp i32 [[ARG]] to float
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR_ELSE]], i32 2, i32 poison, i32 poison, float [[ELSE_ARG]], i32 5)
; CHECK-NEXT:    unreachable
;
entry:
  %cond = icmp ult i32 %arg, 3
  br i1 %cond, label %then, label %else

then:
  %table.0 = getelementptr i32, ptr %table, i32 0
  %cr.then = load i32, ptr %table.0
  %then.arg = add i32 %arg, 1
  call void (...) @lgc.cps.jump(i32 %cr.then, i32 2, i32 poison, i32 poison, i32 %then.arg)
  unreachable

else:
  %table.1 = getelementptr i32, ptr %table, i32 1
  %cr.else = load i32, ptr %table.1
  %else.arg = uitofp i32 %arg to float
  call void (...) @lgc.cps.jump(i32 %cr.else, i32 2, i32 poison, i32 poison, float %else.arg, i32 5)
  unreachable
}

define void @unify_jump_ret(i32 %arg, ptr %table) !lgc.cps !0 !lgc.shaderstage !{i32 7} {
; CHECK-LABEL: define void @unify_jump_ret(
; CHECK-SAME: i32 [[ARG:%.*]], ptr [[TABLE:%.*]]) !lgc.cps [[META1]] !lgc.shaderstage [[META2]] {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[COND:%.*]] = icmp ult i32 [[ARG]], 3
; CHECK-NEXT:    br i1 [[COND]], label %[[THEN:.*]], label %[[ELSE:.*]]
; CHECK:       [[THEN]]:
; CHECK-NEXT:    [[TABLE_0:%.*]] = getelementptr i32, ptr [[TABLE]], i32 0
; CHECK-NEXT:    [[CR_THEN:%.*]] = load i32, ptr [[TABLE_0]], align 4
; CHECK-NEXT:    [[THEN_ARG:%.*]] = add i32 [[ARG]], 1
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR_THEN]], i32 2, i32 poison, i32 poison, i32 [[THEN_ARG]])
; CHECK-NEXT:    unreachable
; CHECK:       [[ELSE]]:
; CHECK-NEXT:    ret void
;
entry:
  %cond = icmp ult i32 %arg, 3
  br i1 %cond, label %then, label %else

then:
  %table.0 = getelementptr i32, ptr %table, i32 0
  %cr.then = load i32, ptr %table.0
  %then.arg = add i32 %arg, 1
  call void (...) @lgc.cps.jump(i32 %cr.then, i32 2, i32 poison, i32 poison, i32 %then.arg)
  unreachable

else:
  ret void
}

!continuation.stackAddrspace = !{!1}

!0 = !{i32 1} ; level 1
!1 = !{i32 5}
;.
; CHECK: [[META1]] = !{i32 1}
; CHECK: [[META2]] = !{i32 7}
;.
