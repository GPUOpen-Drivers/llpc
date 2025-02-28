
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

; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --tool cross-module-inline --version 3
; RUN: cross-module-inline %s %S/inc/link-existing-global-name.ll --link inline_fun | FileCheck %s
;
; Have global with the same name but different types in both modules. Check that linking creates a second global.

@glob = external global float

declare i32 @inline_fun()

define i32 @main() {
; CHECK-LABEL: define i32 @main() {
; CHECK-NEXT:    [[RESULT_I:%.*]] = load i32, ptr @"glob.cloned.{{.*}}", align 4
; CHECK-NEXT:    ret i32 [[RESULT_I]]
;
  %result = call i32 @inline_fun()
  ret i32 %result
}
