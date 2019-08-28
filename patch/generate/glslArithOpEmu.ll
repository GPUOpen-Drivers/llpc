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

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024"
target triple = "spir64-unknown-unknown"

declare float @llvm.amdgcn.fmul.legacy(float, float) #1
declare i32 @llvm.amdgcn.sbfe.i32(i32, i32, i32) #1
declare i32 @llvm.amdgcn.ubfe.i32(i32, i32, i32) #1
declare float @llvm.trunc.f32(float ) #0
declare { i32, i1 } @llvm.uadd.with.overflow.i32(i32, i32) #0
declare { i32, i1 } @llvm.usub.with.overflow.i32(i32, i32) #0
declare float @llvm.exp2.f32(float) #0
declare float @llvm.log2.f32(float) #0
declare float @llvm.sin.f32(float) #0
declare float @llvm.cos.f32(float) #0
declare float @llvm.sqrt.f32(float) #0
declare float @llvm.floor.f32(float) #0
declare float @llvm.pow.f32(float, float) #0
declare float @llvm.minnum.f32(float, float) #0
declare float @llvm.maxnum.f32(float, float) #0
declare float @llvm.fabs.f32(float) #0
declare i32 @llvm.cttz.i32(i32, i1) #0
declare i32 @llvm.ctlz.i32(i32, i1) #0
declare i32 @llvm.amdgcn.sffbh.i32(i32) #1
declare i1 @llvm.amdgcn.class.f32(float, i32) #1
declare float @llvm.amdgcn.frexp.mant.f32(float) #1
declare i32 @llvm.amdgcn.frexp.exp.i32.f32(float) #1
declare i32 @llvm.amdgcn.cvt.pk.u8.f32(float, i32, i32) #1
declare float @llvm.amdgcn.fdiv.fast(float, float) #1
declare float @llvm.amdgcn.fract.f32(float) #1
declare float @llvm.amdgcn.fmed3.f32(float, float, float) #1
declare float @llvm.rint.f32(float) #0
declare float @llvm.fmuladd.f32(float, float, float) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
