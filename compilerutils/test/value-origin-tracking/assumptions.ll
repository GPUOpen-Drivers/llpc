
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

; RUN: opt -passes="value-origin-tracking-test" -S %s | FileCheck %s

declare void @analyze(...)

; Intrinsic to declare value origin assumptions.
; Syntax:
;   call void @assume(%val, [constantOrDynamicValue], i32 dynamicValueByteOffset, [...])
declare void @assume(...)

declare i32 @opaque()

define void @testSimpleAssumptions(i32 %arg) {
; CHECK-LABEL: testSimpleAssumptions

  %opaque = call i32 @opaque()
; CHECK: %opaque = call i32 @opaque()): Dynamic: %opaque = {{.*}} (offset 0)
  call void @analyze(i32 %opaque)

  %opaque.with.constant.assumption = call i32 @opaque()
  call void @assume(i32 %opaque.with.constant.assumption, i32 u0xbeef, i32 0)
; CHECK: %opaque.with.constant.assumption = call i32 @opaque()): Constant: 0xbeef
  call void @analyze(i32 %opaque.with.constant.assumption)

  %opaque.with.dynamic.assumption = call i32 @opaque()
  call void @assume(i32 %opaque.with.dynamic.assumption, i32 %arg, i32 0)
; CHECK: %opaque.with.dynamic.assumption = call i32 @opaque()): Dynamic (argument): i32 %arg (offset 0)
  call void @analyze(i32 %opaque.with.dynamic.assumption)

  %opaque.with.self.assumption = call i32 @opaque()
  call void @assume(i32 %opaque.with.self.assumption, i32 %opaque.with.self.assumption, i32 0)
; CHECK: %opaque.with.self.assumption = call i32 @opaque()): Dynamic: %opaque.with.self.assumption {{.*}} (offset 0)
  call void @analyze(i32 %opaque.with.self.assumption)

  %opaque.with.nested.assumption = call i32 @opaque()
  call void @assume(i32 %opaque.with.nested.assumption, i32 %opaque.with.dynamic.assumption, i32 0)
; CHECK: %opaque.with.nested.assumption = call i32 @opaque()): Dynamic (argument): i32 %arg (offset 0)
  call void @analyze(i32 %opaque.with.nested.assumption)

  %derived = bitcast i32 %opaque.with.nested.assumption to float
; CHECK: %derived = bitcast i32 %opaque.with.nested.assumption to float): Dynamic (argument): i32 %arg (offset 0)
  call void @analyze(float %derived)

  ; Test that we currently don't merge assumptions with our own analysis on the same value:
  ; A trivial assumption can lead to worse results.
  %trivial = bitcast i32 0 to float
  call void @assume(float %trivial, float %trivial, i32 0)
; CHECK: %trivial = bitcast i32 0 to float): Dynamic: %trivial
  call void @analyze(float %trivial)

  ret void
}

declare [3 x i32] @permute([3 x i32] %arr)

; Test assumptions on larger types with nontrivial offsets
; Add assumptions assuming that @permute permutes the input array.
; After three rounds, we should get back the original one.
define void @testAssumptionsWithOffsets([3 x i32] %arg) {
; CHECK-LABEL: testAssumptionsWithOffsets
  %permuted.0 = call [3 x i32] @permute([3 x i32] %arg)
  call void @assume([3 x i32] %permuted.0, [3 x i32] %arg, i32 4, [3 x i32] %arg, i32 8, [3 x i32] %arg, i32 0)
  %permuted.1 = call [3 x i32] @permute([3 x i32] %permuted.0)
  call void @assume([3 x i32] %permuted.1, [3 x i32] %permuted.0, i32 4, [3 x i32] %permuted.0, i32 8, [3 x i32] %permuted.0, i32 0)
; CHECK: %permuted.1 = {{.*}}: Dynamic (argument): [3 x i32] %arg (offset 8); Dynamic (argument): [3 x i32] %arg (offset 0); Dynamic (argument): [3 x i32] %arg (offset 4)
  call void @analyze([3 x i32] %permuted.1)
  %permuted.final = call [3 x i32] @permute([3 x i32] %permuted.1)
  call void @assume([3 x i32] %permuted.final, [3 x i32] %permuted.1, i32 4, [3 x i32] %permuted.1, i32 8, [3 x i32] %permuted.1, i32 0)
; CHECK: %permuted.final = {{.*}}: Dynamic (argument): [3 x i32] %arg (offset 0); Dynamic (argument): [3 x i32] %arg (offset 4); Dynamic (argument): [3 x i32] %arg (offset 8)
  call void @analyze([3 x i32] %permuted.final)

  ret void
}
