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

declare half @llvm.trunc.f16(half) #0
declare half @llvm.fabs.f16(half) #0
declare half @llvm.sqrt.f16(half) #0
declare half @llvm.floor.f16(half) #0
declare half @llvm.exp2.f16(half) #0
declare half @llvm.log2.f16(half) #0
declare half @llvm.sin.f16(half) #0
declare half @llvm.cos.f16(half) #0
declare float @llpc.asin.f32(float) #0
declare float @llpc.acos.f32(float) #0
declare half @llvm.minnum.f16(half, half) #0
declare half @llvm.maxnum.f16(half, half) #0
declare half @llvm.fmuladd.f16(half, half, half) #0
declare i1 @llvm.amdgcn.class.f16(half, i32) #1
declare half @llvm.amdgcn.fract.f16(half) #1
declare half @llvm.amdgcn.fmed3.f16(half, half, half) #1
declare half @llvm.rint.f16(half) #0
declare i16 @llvm.amdgcn.frexp.exp.i16.f16(half) #1
declare half @llvm.amdgcn.frexp.mant.f16(half) #1
declare <2 x half> @llvm.amdgcn.cvt.pkrtz(float, float) #1
declare i32 @llvm.amdgcn.ubfe.i32(i32, i32, i32) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
