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
declare ptr addrspace(32) @lgc.cps.alloc(i32)
declare void @lgc.cps.free(i32)
declare i32 @lgc.cps.as.continuation.reference(ptr)
declare ptr addrspace(32) @lgc.cps.peek(i32)
declare ptr addrspace(32) @lgc.cps.get.vsp()
declare i32 @lgc.cps.get.dummy.index(i32)
declare void @lgc.cps.complete()

%_rgen_1.Frame = type { ptr addrspace(5), ptr addrspace(5), i32 }

define void @test.0() !lgc.cps !{i32 1} !lgc.shaderstage !{i32 7} !continuation !{ptr @test.0} {
; CHECK-LABEL: define void @test.0(
; CHECK-SAME: i32 [[CSPINIT:%.*]]) !lgc.cps [[META1:![0-9]+]] !lgc.shaderstage [[META2:![0-9]+]] !continuation [[META3:![0-9]+]] !continuation.state [[META4:![0-9]+]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = add i32 [[TMP0]], 12
; CHECK-NEXT:    store i32 [[TMP1]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[TMP0]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP2]], i32 0
; CHECK-NEXT:    store i32 333, ptr addrspace(5) [[TMP3]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP0]], 4
; CHECK-NEXT:    [[TMP5:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP6:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP5]], i32 0
; CHECK-NEXT:    store i32 111, ptr addrspace(5) [[TMP6]], align 4
; CHECK-NEXT:    [[TMP7:%.*]] = add i32 [[TMP0]], 9
; CHECK-NEXT:    [[TMP8:%.*]] = inttoptr i32 [[TMP7]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP9:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP8]], i32 0
; CHECK-NEXT:    store i8 99, ptr addrspace(5) [[TMP9]], align 1
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
; CHECK-NEXT:    [[TMP10:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP10]], i32 poison, i32 6, i32 [[TMP7]], i32 [[TMP4]])
; CHECK-NEXT:    unreachable
;
  %mem = call ptr addrspace(32) @lgc.cps.alloc(i32 10)   ; round up to 12 during lowering

  store i32 333, ptr addrspace(32) %mem

  %p1 = getelementptr i32, ptr addrspace(32) %mem, i32 1
  store i32 111, ptr addrspace(32) %p1

  %p2 = getelementptr i8, ptr addrspace(32) %mem, i32 9
  store i8 99, ptr addrspace(32) %p2

  %q1 = ptrtoint ptr addrspace(32) %p1 to i32

  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, i32 poison, i32 poison, i32 6, ptr addrspace(32) %p2, i32 %q1)
  unreachable
}

define void @test.1(ptr addrspace(32) %p2, i32 %q1) !lgc.cps !{i32 1} !lgc.shaderstage !{i32 7} !continuation !{ptr @test.0} {
; CHECK-LABEL: define void @test.1(
; CHECK-SAME: i32 [[CSPINIT:%.*]], i32 [[P2:%.*]], i32 [[Q1:%.*]]) !lgc.cps [[META1]] !lgc.shaderstage [[META2]] !continuation [[META5:![0-9]+]] !continuation.state [[META4]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = inttoptr i32 [[Q1]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP1:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP0]], i32 0
; CHECK-NEXT:    [[N111:%.*]] = load i32, ptr addrspace(5) [[TMP1]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[P2]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP2]], i32 0
; CHECK-NEXT:    [[N99:%.*]] = load i8, ptr addrspace(5) [[TMP3]], align 1
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @test.2)
; CHECK-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP4]], i32 poison, i32 4)
; CHECK-NEXT:    unreachable
;
  %p1 = inttoptr i32 %q1 to ptr addrspace(32)
  %n111 = load i32, ptr addrspace(32) %p1
  %n99 = load i8, ptr addrspace(32) %p2

  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @test.2)
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, i32 poison, i32 poison, i32 4)
  unreachable
}

define void @test.2() !lgc.cps !{i32 1} !lgc.shaderstage !{i32 7} !continuation !{ptr @test.0} {
; CHECK-LABEL: define void @test.2(
; CHECK-SAME: i32 [[CSPINIT:%.*]]) !lgc.cps [[META1]] !lgc.shaderstage [[META2]] !continuation [[META6:![0-9]+]] !continuation.state [[META4]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = add i32 [[TMP0]], -12
; CHECK-NEXT:    [[TMP2:%.*]] = inttoptr i32 [[TMP1]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP2]], i32 0
; CHECK-NEXT:    [[N333:%.*]] = load i32, ptr addrspace(5) [[TMP3]], align 4
; CHECK-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP5:%.*]] = add i32 [[TMP4]], -12
; CHECK-NEXT:    store i32 [[TMP5]], ptr [[CSP]], align 4
; CHECK-NEXT:    ret void
;
  %mem = call ptr addrspace(32) @lgc.cps.peek(i32 10)    ; round up to 12 during lowering

  %n333 = load i32, ptr addrspace(32) %mem

  call void @lgc.cps.free(i32 10)   ; round up to 12 during lowering

  call void @lgc.cps.complete()
  unreachable
}

; Dummy test to show behavior with lowering of non-constant GEP indices.
define void @test.gep() !lgc.cps !{i32 1} !lgc.shaderstage !{i32 7} !continuation !{ptr @test.0} {
; CHECK-LABEL: define void @test.gep(
; CHECK-SAME: i32 [[CSPINIT:%.*]]) !lgc.cps [[META1]] !lgc.shaderstage [[META2]] !continuation [[META7:![0-9]+]] !continuation.state [[META4]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = add i32 [[TMP0]], 12
; CHECK-NEXT:    store i32 [[TMP1]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[STACK_EL0:%.*]] = call i32 @lgc.cps.get.dummy.index(i32 0)
; CHECK-NEXT:    [[TMP2:%.*]] = mul i32 [[STACK_EL0]], 24
; CHECK-NEXT:    [[TMP3:%.*]] = add i32 [[TMP0]], [[TMP2]]
; CHECK-NEXT:    [[TMP4:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP5:%.*]] = inttoptr i32 [[TMP3]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP6:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP5]], i32 0
; CHECK-NEXT:    store i32 [[TMP4]], ptr addrspace(5) [[TMP6]], align 4
; CHECK-NEXT:    [[STACK_EL1:%.*]] = call i32 @lgc.cps.get.dummy.index(i32 1)
; CHECK-NEXT:    [[TMP7:%.*]] = mul i32 [[STACK_EL1]], 24
; CHECK-NEXT:    [[TMP8:%.*]] = add i32 [[TMP0]], [[TMP7]]
; CHECK-NEXT:    [[TMP9:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP10:%.*]] = add i32 [[TMP9]], -4
; CHECK-NEXT:    [[TMP11:%.*]] = inttoptr i32 [[TMP8]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP12:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP11]], i32 0
; CHECK-NEXT:    store i32 [[TMP10]], ptr addrspace(5) [[TMP12]], align 4
; CHECK-NEXT:    [[STACK_EL2:%.*]] = call i32 @lgc.cps.get.dummy.index(i32 2)
; CHECK-NEXT:    [[STACK_EL2_DIV:%.*]] = sdiv i32 [[STACK_EL2]], 2
; CHECK-NEXT:    [[TMP13:%.*]] = add i32 [[TMP0]], 8
; CHECK-NEXT:    [[TMP14:%.*]] = mul i32 [[STACK_EL2_DIV]], 24
; CHECK-NEXT:    [[TMP15:%.*]] = add i32 [[TMP13]], [[TMP14]]
; CHECK-NEXT:    [[TMP16:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP17:%.*]] = add i32 [[TMP16]], -8
; CHECK-NEXT:    [[TMP18:%.*]] = inttoptr i32 [[TMP15]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP19:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP18]], i32 0
; CHECK-NEXT:    store i32 [[TMP17]], ptr addrspace(5) [[TMP19]], align 4
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
; CHECK-NEXT:    [[TMP20:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP20]], i32 poison, i32 5, i32 [[TMP17]], i32 [[TMP17]])
; CHECK-NEXT:    unreachable
;
  %mem = call ptr addrspace(32) @lgc.cps.alloc(i32 10)   ; round up to 12 during lowering

  %stack.el0 = call i32 @lgc.cps.get.dummy.index(i32 0)
  %1 = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %mem, i32 %stack.el0
  %vsp = call ptr addrspace(32) @lgc.cps.get.vsp()
  %vsp.i = ptrtoint ptr addrspace(32) %vsp to i32
  store i32 %vsp.i, ptr addrspace(32) %1

  %stack.el1 = call i32 @lgc.cps.get.dummy.index(i32 1)
  %2 = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %mem, i32 %stack.el1
  %vsp.2 = call ptr addrspace(32) @lgc.cps.peek(i32 4)
  %vsp.2.i = ptrtoint ptr addrspace(32) %vsp.2 to i32
  store i32 %vsp.2.i, ptr addrspace(32) %2

  %stack.el2 = call i32 @lgc.cps.get.dummy.index(i32 2)
  %stack.el2.div = sdiv i32 %stack.el2, 2
  %3 = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %mem, i32 %stack.el2.div, i32 1
  %vsp.3 = call ptr addrspace(32) @lgc.cps.peek(i32 8)
  %vsp.3.i = ptrtoint ptr addrspace(32) %vsp.3 to i32
  store i32 %vsp.3.i, ptr addrspace(32) %3

  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, i32 poison, i32 poison, i32 5, ptr addrspace(32) %vsp.3, i32 %vsp.3.i)
  unreachable
}

; Dummy test to show behavior with lowering of nested GEPs.
define void @test.nested.gep() !lgc.cps !{i32 1} !lgc.shaderstage !{i32 7} !continuation !{ptr @test.0} {
; CHECK-LABEL: define void @test.nested.gep(
; CHECK-SAME: i32 [[CSPINIT:%.*]]) !lgc.cps [[META1]] !lgc.shaderstage [[META2]] !continuation [[META8:![0-9]+]] !continuation.state [[META4]] {
; CHECK-NEXT:  [[ALLOCASPILLBB:.*:]]
; CHECK-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; CHECK-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP0:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = add i32 [[TMP0]], 12
; CHECK-NEXT:    store i32 [[TMP1]], ptr [[CSP]], align 4
; CHECK-NEXT:    [[STACK_EL0:%.*]] = call i32 @lgc.cps.get.dummy.index(i32 0)
; CHECK-NEXT:    [[TMP2:%.*]] = mul i32 [[STACK_EL0]], 24
; CHECK-NEXT:    [[TMP3:%.*]] = add i32 [[TMP0]], [[TMP2]]
; CHECK-NEXT:    [[TMP4:%.*]] = add i32 [[TMP3]], 16
; CHECK-NEXT:    [[TMP5:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    [[TMP6:%.*]] = inttoptr i32 [[TMP4]] to ptr addrspace(5)
; CHECK-NEXT:    [[TMP7:%.*]] = getelementptr i8, ptr addrspace(5) [[TMP6]], i32 0
; CHECK-NEXT:    store i32 [[TMP5]], ptr addrspace(5) [[TMP7]], align 4
; CHECK-NEXT:    [[CR:%.*]] = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
; CHECK-NEXT:    [[TMP8:%.*]] = load i32, ptr [[CSP]], align 4
; CHECK-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, i32 [[TMP8]], i32 poison, i32 3, i32 [[TMP5]], i32 [[TMP5]])
; CHECK-NEXT:    unreachable
;
  %mem = call ptr addrspace(32) @lgc.cps.alloc(i32 10)   ; round up to 12 during lowering

  %stack.el0 = call i32 @lgc.cps.get.dummy.index(i32 0)
  %gep.base = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %mem, i32 %stack.el0
  %1 = getelementptr inbounds %_rgen_1.Frame, ptr addrspace(32) %gep.base, i32 0, i32 2
  %vsp = call ptr addrspace(32) @lgc.cps.get.vsp()
  %vsp.i = ptrtoint ptr addrspace(32) %vsp to i32
  store i32 %vsp.i, ptr addrspace(32) %1

  %cr = call i32 @lgc.cps.as.continuation.reference(ptr @test.1)
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, i32 poison, i32 poison, i32 3, ptr addrspace(32) %vsp, i32 %vsp.i)
  unreachable
}

!continuation.stackAddrspace = !{!0}

!0 = !{i32 5}
;.
; CHECK: [[META1]] = !{i32 1}
; CHECK: [[META2]] = !{i32 7}
; CHECK: [[META3]] = !{ptr @test.0}
; CHECK: [[META4]] = !{i32 0}
; CHECK: [[META5]] = !{ptr @test.1}
; CHECK: [[META6]] = !{ptr @test.2}
; CHECK: [[META7]] = !{ptr @test.gep}
; CHECK: [[META8]] = !{ptr @test.nested.gep}
;.
