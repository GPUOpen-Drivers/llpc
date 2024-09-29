; RUN: opt -passes="value-origin-tracking-test" -S %s -value-origin-tracking-test-max-bytes-per-value=4 | FileCheck %s --check-prefix=CHECK-SMALL
; RUN: opt -passes="value-origin-tracking-test" -S %s -value-origin-tracking-test-max-bytes-per-value=1024 | FileCheck %s --check-prefix=CHECK-HUGE

declare void @analyze(...)

define void @test() {
; CHECK-LABEL: test
  %arr = insertvalue [256 x i32] poison, i32 7, 255
  %val = extractvalue [256 x i32] %arr, 255
; CHECK-SMALL: (  %val = extractvalue [256 x i32] %arr, 255): Dynamic:   %val = extractvalue [256 x i32] %arr, 255 (offset 0)
; CHECK-HUGE:  (  %val = extractvalue [256 x i32] %arr, 255): Constant: 0x7
  call void @analyze(i32 %val)
  ret void
}
