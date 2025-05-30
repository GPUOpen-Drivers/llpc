; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 5
; RUN: opt -o - -passes='cleanup-continuations' %s -S | FileCheck --check-prefixes=CHECK %s
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

%_rgen_1.Frame = type { ptr addrspace(1), ptr addrspace(1), i32 }

declare void @lgc.cps.jump(...) #0

declare ptr addrspace(32) @lgc.cps.alloc(i32)

declare void @lgc.cps.free(i32)

declare i32 @lgc.cps.as.continuation.reference(ptr)

declare ptr addrspace(32) @lgc.cps.peek(i32)

declare ptr addrspace(32) @lgc.cps.get.vsp()

declare i32 @lgc.cps.get.dummy.index(i32)

declare void @lgc.cps.complete()

declare i64 @_cont_GetContinuationStackGlobalMemBase()

define { ptr, ptr } @test.0(ptr %0) !lgc.cps !1 !lgc.rt.shaderstage !2 !continuation !3 {
; CHECK-LABEL: define void @test.0(
; CHECK-SAME: ) !lgc.cps [[META0:![0-9]+]] !lgc.rt.shaderstage [[META1:![0-9]+]] !continuation [[META2:![0-9]+]] !continuation.state [[META3:![0-9]+]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    [[TMP2:%.*]] = call i64 @_cont_GetContinuationStackGlobalMemBase()
; CHECK-NEXT:    [[TMP3:%.*]] = inttoptr i64 [[TMP2]] to ptr addrspace(1)
; CHECK-NEXT:    [[TMP0:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = add i32 [[TMP0]], 12
; CHECK-NEXT:    store i32 [[TMP1]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP5:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP3]], i32 [[TMP0]]
; CHECK-NEXT:    store i32 333, ptr addrspace(1) [[TMP5]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP0]], 4
; CHECK-NEXT:    [[TMP6:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP3]], i32 [[TMP4]]
; CHECK-NEXT:    store i32 111, ptr addrspace(1) [[TMP6]], align 4
; CHECK-NEXT:    [[TMP7:%.*]] = add i32 [[TMP0]], 9
; CHECK-NEXT:    [[TMP8:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP3]], i32 [[TMP7]]
; CHECK-NEXT:    store i8 99, ptr addrspace(1) [[TMP8]], align 1
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
; CHECK-NEXT:    [[TMP10:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP10]], i32 poison, i32 6, i32 [[TMP7]], i32 [[TMP4]])
; CHECK-NEXT:    unreachable
;
AllocaSpillBB:
  %mem = call ptr addrspace(32) @lgc.cps.alloc(i32 10)
  store i32 333, ptr addrspace(32) %mem, align 4
  %p1 = getelementptr i32, ptr addrspace(32) %mem, i32 1
  store i32 111, ptr addrspace(32) %p1, align 4
  %p2 = getelementptr i8, ptr addrspace(32) %mem, i32 9
  store i8 99, ptr addrspace(32) %p2, align 1
  %q1 = ptrtoint ptr addrspace(32) %p1 to i32
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, i32 poison, i32 poison, i32 6, ptr addrspace(32) %p2, i32 %q1)
  unreachable
}

define { ptr, ptr } @test.1(ptr addrspace(32) %p2, i32 %q1, ptr %0) !lgc.cps !1 !lgc.rt.shaderstage !2 !continuation !4 {
; CHECK-LABEL: define void @test.1(
; CHECK-SAME: i32 [[P2:%.*]], i32 [[Q1:%.*]]) !lgc.cps [[META0]] !lgc.rt.shaderstage [[META1]] !continuation [[META4:![0-9]+]] !continuation.state [[META3]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    [[TMP0:%.*]] = call i64 @_cont_GetContinuationStackGlobalMemBase()
; CHECK-NEXT:    [[TMP1:%.*]] = inttoptr i64 [[TMP0]] to ptr addrspace(1)
; CHECK-NEXT:    [[TMP2:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP1]], i32 [[Q1]]
; CHECK-NEXT:    [[N111:%.*]] = load i32, ptr addrspace(1) [[TMP2]], align 4
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP1]], i32 [[P2]]
; CHECK-NEXT:    [[N99:%.*]] = load i8, ptr addrspace(1) [[TMP3]], align 1
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @test.2)
; CHECK-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP4]], i32 poison, i32 poison)
; CHECK-NEXT:    unreachable
;
AllocaSpillBB:
  %p1 = inttoptr i32 %q1 to ptr addrspace(32)
  %n111 = load i32, ptr addrspace(32) %p1, align 4
  %n99 = load i8, ptr addrspace(32) %p2, align 1
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @test.2)
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, i32 poison, i32 poison, i32 poison)
  unreachable
}

define { ptr, ptr } @test.2(ptr %0) !lgc.cps !1 !lgc.rt.shaderstage !2 !continuation !5 {
; CHECK-LABEL: define void @test.2(
; CHECK-SAME: ) !lgc.cps [[META0]] !lgc.rt.shaderstage [[META1]] !continuation [[META5:![0-9]+]] !continuation.state [[META3]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    [[TMP2:%.*]] = call i64 @_cont_GetContinuationStackGlobalMemBase()
; CHECK-NEXT:    [[TMP3:%.*]] = inttoptr i64 [[TMP2]] to ptr addrspace(1)
; CHECK-NEXT:    [[TMP0:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = add i32 [[TMP0]], -12
; CHECK-NEXT:    [[TMP6:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP3]], i32 [[TMP1]]
; CHECK-NEXT:    [[N333:%.*]] = load i32, ptr addrspace(1) [[TMP6]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], -12
; CHECK-NEXT:    store i32 [[TMP5]], ptr [[CSP]], align 4
; CHECK-NEXT:    ret void
;
AllocaSpillBB:
  %mem = call ptr addrspace(32) @lgc.cps.peek(i32 10)
  %n333 = load i32, ptr addrspace(32) %mem, align 4
  call void @lgc.cps.free(i32 10)
  call void @lgc.cps.complete()
  unreachable
}

define { ptr, ptr } @test.gep(ptr %0) !lgc.cps !1 !lgc.rt.shaderstage !2 !continuation !6 {
; CHECK-LABEL: define void @test.gep(
; CHECK-SAME: ) !lgc.cps [[META0]] !lgc.rt.shaderstage [[META1]] !continuation [[META6:![0-9]+]] !continuation.state [[META3]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    [[TMP5:%.*]] = call i64 @_cont_GetContinuationStackGlobalMemBase()
; CHECK-NEXT:    [[TMP6:%.*]] = inttoptr i64 [[TMP5]] to ptr addrspace(1)
; CHECK-NEXT:    [[TMP0:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = add i32 [[TMP0]], 12
; CHECK-NEXT:    store i32 [[TMP1]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[STACK_EL0:%.*]] = call i32 @lgc.cps.get.dummy.index(i32 0)
; CHECK-NEXT:    [[TMP2:%.*]] = mul i32 [[STACK_EL0]], 24
; CHECK-NEXT:    [[TMP3:%.*]] = add i32 [[TMP0]], [[TMP2]]
; CHECK-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP11:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP6]], i32 [[TMP3]]
; CHECK-NEXT:    store i32 [[TMP4]], ptr addrspace(1) [[TMP11]], align 4
; CHECK-NEXT:    [[STACK_EL1:%.*]] = call i32 @lgc.cps.get.dummy.index(i32 1)
; CHECK-NEXT:    [[TMP7:%.*]] = mul i32 [[STACK_EL1]], 24
; CHECK-NEXT:    [[TMP8:%.*]] = add i32 [[TMP0]], [[TMP7]]
; CHECK-NEXT:    [[TMP9:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP10:%.*]] = add i32 [[TMP9]], -4
; CHECK-NEXT:    [[TMP12:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP6]], i32 [[TMP8]]
; CHECK-NEXT:    store i32 [[TMP10]], ptr addrspace(1) [[TMP12]], align 4
; CHECK-NEXT:    [[STACK_EL2:%.*]] = call i32 @lgc.cps.get.dummy.index(i32 2)
; CHECK-NEXT:    [[STACK_EL2_DIV:%.*]] = sdiv i32 [[STACK_EL2]], 2
; CHECK-NEXT:    [[TMP13:%.*]] = add i32 [[TMP0]], 8
; CHECK-NEXT:    [[TMP14:%.*]] = mul i32 [[STACK_EL2_DIV]], 24
; CHECK-NEXT:    [[TMP15:%.*]] = add i32 [[TMP13]], [[TMP14]]
; CHECK-NEXT:    [[TMP16:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP17:%.*]] = add i32 [[TMP16]], -8
; CHECK-NEXT:    [[TMP18:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP6]], i32 [[TMP15]]
; CHECK-NEXT:    store i32 [[TMP17]], ptr addrspace(1) [[TMP18]], align 4
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
; CHECK-NEXT:    [[TMP20:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP20]], i32 poison, i32 5, i32 [[TMP17]], i32 [[TMP17]])
; CHECK-NEXT:    unreachable
;
AllocaSpillBB:
  %mem = call ptr addrspace(32) @lgc.cps.alloc(i32 10)
  %stack.el0 = call i32 @lgc.cps.get.dummy.index(i32 0)
  %1 = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %mem, i32 %stack.el0
  %vsp = call ptr addrspace(32) @lgc.cps.get.vsp()
  %vsp.i = ptrtoint ptr addrspace(32) %vsp to i32
  store i32 %vsp.i, ptr addrspace(32) %1, align 4
  %stack.el1 = call i32 @lgc.cps.get.dummy.index(i32 1)
  %2 = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %mem, i32 %stack.el1
  %vsp.2 = call ptr addrspace(32) @lgc.cps.peek(i32 4)
  %vsp.2.i = ptrtoint ptr addrspace(32) %vsp.2 to i32
  store i32 %vsp.2.i, ptr addrspace(32) %2, align 4
  %stack.el2 = call i32 @lgc.cps.get.dummy.index(i32 2)
  %stack.el2.div = sdiv i32 %stack.el2, 2
  %3 = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %mem, i32 %stack.el2.div, i32 1
  %vsp.3 = call ptr addrspace(32) @lgc.cps.peek(i32 8)
  %vsp.3.i = ptrtoint ptr addrspace(32) %vsp.3 to i32
  store i32 %vsp.3.i, ptr addrspace(32) %3, align 4
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, i32 poison, i32 poison, i32 5, ptr addrspace(32) %vsp.3, i32 %vsp.3.i)
  unreachable
}

define { ptr, ptr } @test.nested.gep(ptr %0) !lgc.cps !1 !lgc.rt.shaderstage !2 !continuation !7 {
; CHECK-LABEL: define void @test.nested.gep(
; CHECK-SAME: ) !lgc.cps [[META0]] !lgc.rt.shaderstage [[META1]] !continuation [[META7:![0-9]+]] !continuation.state [[META3]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    [[TMP6:%.*]] = call i64 @_cont_GetContinuationStackGlobalMemBase()
; CHECK-NEXT:    [[TMP7:%.*]] = inttoptr i64 [[TMP6]] to ptr addrspace(1)
; CHECK-NEXT:    [[TMP0:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = add i32 [[TMP0]], 12
; CHECK-NEXT:    store i32 [[TMP1]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[STACK_EL0:%.*]] = call i32 @lgc.cps.get.dummy.index(i32 0)
; CHECK-NEXT:    [[TMP2:%.*]] = mul i32 [[STACK_EL0]], 24
; CHECK-NEXT:    [[TMP3:%.*]] = add i32 [[TMP0]], [[TMP2]]
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP3]], 16
; CHECK-NEXT:    [[TMP5:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr i8, ptr addrspace(1) [[TMP7]], i32 [[TMP4]]
; CHECK-NEXT:    store i32 [[TMP5]], ptr addrspace(1) [[TMP9]], align 4
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
; CHECK-NEXT:    [[TMP8:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP8]], i32 poison, i32 4, i32 [[TMP5]], i32 [[TMP5]])
; CHECK-NEXT:    unreachable
;
AllocaSpillBB:
  %mem = call ptr addrspace(32) @lgc.cps.alloc(i32 10)
  %stack.el0 = call i32 @lgc.cps.get.dummy.index(i32 0)
  %gep.base = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %mem, i32 %stack.el0
  %1 = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %gep.base, i32 0, i32 2
  %vsp = call ptr addrspace(32) @lgc.cps.get.vsp()
  %vsp.i = ptrtoint ptr addrspace(32) %vsp to i32
  store i32 %vsp.i, ptr addrspace(32) %1, align 4
  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, i32 poison, i32 poison, i32 4, ptr addrspace(32) %vsp, i32 %vsp.i)
  unreachable
}

declare !continuation !3 { ptr, ptr } @continuation.prototype.test.0(ptr, i1)

declare ptr @continuation.malloc(i32)

declare void @continuation.free(ptr)

declare token @llvm.coro.id.retcon(i32, i32, ptr, ptr, ptr, ptr) #1

declare ptr @llvm.coro.begin(token, ptr writeonly) #1

declare !continuation !4 { ptr, ptr } @continuation.prototype.test.1(ptr, i1)

declare !continuation !5 { ptr, ptr } @continuation.prototype.test.2(ptr, i1)

declare !continuation !6 { ptr, ptr } @continuation.prototype.test.gep(ptr, i1)

declare !continuation !7 { ptr, ptr } @continuation.prototype.test.nested.gep(ptr, i1)

attributes #0 = { noreturn }
attributes #1 = { nounwind }

!continuation.stackAddrspace = !{!0}

!0 = !{i32 1}
!1 = !{i32 1}
!2 = !{i32 7}
!3 = !{ptr @test.0}
!4 = !{ptr @test.1}
!5 = !{ptr @test.2}
!6 = !{ptr @test.gep}
!7 = !{ptr @test.nested.gep}
;.
; CHECK: [[META0]] = !{i32 1}
; CHECK: [[META1]] = !{i32 7}
; CHECK: [[META2]] = !{ptr @test.0}
; CHECK: [[META3]] = !{i32 0}
; CHECK: [[META4]] = !{ptr @test.1}
; CHECK: [[META5]] = !{ptr @test.2}
; CHECK: [[META6]] = !{ptr @test.gep}
; CHECK: [[META7]] = !{ptr @test.nested.gep}
;.
