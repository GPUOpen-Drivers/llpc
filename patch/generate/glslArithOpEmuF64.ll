;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 ;
 ;  Permission is hereby granted, free of charge, to any person obtaining a copy
 ;  of this software and associated documentation files (the "Software"), to deal
 ;  in the Software without restriction, including without limitation the rights
 ;  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 ;  copies of the Software, and to permit persons to whom the Software is
 ;  furnished to do so, subject to the following conditions:
 ;
 ;  The above copyright notice and this permission notice shall be included in all
 ;  copies or substantial portions of the Software.
 ;
 ;  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ;  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ;  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 ;  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 ;  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 ;  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 ;  SOFTWARE.
 ;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

target datalayout = "e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5"

; =====================================================================================================================
; >>>  Common Functions
; =====================================================================================================================

; GLSL: double mod(double, double)
define double @llpc.mod.f64(double %x, double %y) #0
{
    %1 = fdiv double 1.0,%y
    %2 = fmul double %x, %1
    %3 = call double @llvm.floor.f64(double %2)
    %4 = fmul double %y, %3
    %5 = fsub double %x, %4
    ret double %5
}

; GLSL: bool isinf(double)
define i1 @llpc.isinf.f64(double %x) #0
{
    ; 0x004: negative infinity; 0x200: positive infinity
    %1 = call i1 @llvm.amdgcn.class.f64(double %x, i32 516)
    ret i1 %1
}

; GLSL: bool isnan(double)
define i1 @llpc.isnan.f64(double %x) #0
{
    ; 0x001: signaling NaN, 0x002: quiet NaN
    %1 = call i1 @llvm.amdgcn.class.f64(double %x, i32 3)
    ret i1 %1
}

declare double @llvm.trunc.f64(double ) #0
declare double @llvm.fabs.f64(double) #0
declare double @llvm.sqrt.f64(double) #0
declare double @llvm.floor.f64(double) #0
declare double @llvm.minnum.f64(double, double) #0
declare double @llvm.maxnum.f64(double, double) #0
declare double @llvm.amdgcn.frexp.mant.f64(double) #1
declare i1 @llvm.amdgcn.class.f64(double, i32) #1
declare i32 @llvm.amdgcn.frexp.exp.i32.f64(double %x) #1
declare double @llvm.amdgcn.fract.f64(double) #1
declare double @llvm.amdgcn.fmed3.f64(double, double, double) #1
declare double @llvm.rint.f64(double) #0
declare double @llvm.fmuladd.f64(double, double, double) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
