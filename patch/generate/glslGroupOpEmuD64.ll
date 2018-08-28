;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2018 Advanced Micro Devices, Inc. All Rights Reserved.
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

; =====================================================================================================================
; >>>  Shader Invocation Group Functions
; =====================================================================================================================

; GLSL: identity (64-bit)
define spir_func i64 @llpc.subgroup.identity.i64(i32 %binaryOp)
{
.entry:
    switch i32 %binaryOp, label %.default [i32 0,  label %.iadd
                                           i32 1,  label %.imul
                                           i32 2,  label %.smin
                                           i32 3,  label %.smax
                                           i32 4,  label %.umin
                                           i32 5,  label %.umax
                                           i32 6,  label %.and
                                           i32 7,  label %.or
                                           i32 8,  label %.xor
                                           i32 9,  label %.fmul
                                           i32 10, label %.fmin
                                           i32 11, label %.fmax
                                           i32 12, label %.fadd ]

.iadd:
    ret i64 0
.imul:
    ret i64 1
.smin:
    ; 0x7FFF FFFF FFFF FFFF
    ret i64 9223372036854775807
.smax:
    ; 0x8000 0000 0000 0000
    ret i64 -9223372036854775808
.umin:
    ; 0xFFFF FFFF FFFF FFFF
    ret i64 -1
.umax:
    ret i64 0
.and:
    ret i64 0
.or:
    ret i64 0
.xor:
    ret i64 0
.fadd:
    ret i64 0
.fmul:
    ; ‭3FF0,0000,0000,0000, 1.0
    ret i64 4607182418800017408
.fmin:
    ; 7FF0,0000,0000,0000, 1.#INF00E+000
    ret i64 9218868437227405312
.fmax:
    ; FFF0,0000,0000,0000, -1.#INF00E+000
    ret i64 -4503599627370496
.default:
    ret i64 0
}

; GLSL: x [binary] y (int64_t/uint64_t)
define spir_func i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %x, i64 %y)
{
.entry:
    %x64 = bitcast i64 %x to double
    %y64 = bitcast i64 %y to double
    switch i32 %binaryOp, label %.default [i32 0,  label %.iadd
                                           i32 1,  label %.imul
                                           i32 2,  label %.smin
                                           i32 3,  label %.smax
                                           i32 4,  label %.umin
                                           i32 5,  label %.umax
                                           i32 6,  label %.and
                                           i32 7,  label %.or
                                           i32 8,  label %.xor
                                           i32 9,  label %.fmul
                                           i32 10, label %.fmin
                                           i32 11, label %.fmax
                                           i32 12, label %.fadd ]

.iadd:
    %i0 = add i64 %x, %y
    ret i64 %i0
.imul:
    %i1 = mul i64 %x, %y
    ret i64 %i1
.smin:
    %i2 = call i64 @llpc.sminnum.i64(i64 %x, i64 %y)
    ret i64 %i2
.smax:
    %i3 = call i64 @llpc.smaxnum.i64(i64 %x, i64 %y)
    ret i64 %i3
.umin:
    %i4 = call i64 @llpc.uminnum.i64(i64 %x, i64 %y)
    ret i64 %i4
.umax:
    %i5 = call i64 @llpc.umaxnum.i64(i64 %x, i64 %y)
    ret i64 %i5
.and:
    ret i64 0
.or:
    ret i64 0
.xor:
    ret i64 0
.fadd:
    %f0 = fadd double %x64, %y64
    %iv0 = bitcast double %f0 to i64
    ret i64 %iv0
.fmul:
    %f1 = fmul double %x64, %y64
    %iv1 = bitcast double %f1 to i64
    ret i64 %iv1
.fmin:
    %f2 = call double @llvm.minnum.f64(double %x64, double %y64)
    %iv2 = bitcast double %f2 to i64
    ret i64 %iv2
.fmax:
    %f3 = call double @llvm.maxnum.f64(double %x64, double %y64)
    %iv3 = bitcast double %f3 to i64
    ret i64 %iv3
.default:
    ret i64 0
}

; Set values to all inactive lanes (64 bit)
define spir_func i64 @llpc.subgroup.set.inactive.i64(i32 %binaryOp, i64 %value)
{
    %identity = call i64 @llpc.subgroup.identity.i64(i32 %binaryOp)
    %activeValue = call i64 @llvm.amdgcn.set.inactive.i64(i64 %value, i64 %identity)

    ret i64 %activeValue
}

; Emulate ISA: v_cndmask_b32 (64-bit)
define spir_func i64 @llpc.cndmask.i64(i64 %tidmask, i64 %mask, i64 %src0, i64 %src1)
{
    %1 = and i64 %tidmask, %mask
    %2 = icmp ne i64 %1, 0
    %3 = select i1 %2, i64 %src1, i64 %src0

    ret i64 %3
}

; Performs ds_swizzle on 64-bit data
define spir_func i64 @llpc.swizzle.i64(i64 %value, i32 %offset)
{
    %value.v2 = bitcast i64 %value to <2 x i32>
    %1 = extractelement <2 x i32> %value.v2, i32 0
    %2 = extractelement <2 x i32> %value.v2, i32 1
    %3 = call i32 @llvm.amdgcn.ds.swizzle(i32 %1, i32 %offset)
    %4 = call i32 @llvm.amdgcn.ds.swizzle(i32 %2, i32 %offset)
    %5 = insertelement <2 x i32> undef, i32 %3, i32 0
    %6 = insertelement <2 x i32> %5, i32 %4, i32 1
    %7 = bitcast <2 x i32> %6 to i64

    ret i64 %7
}

; Performs readlane on 64-bit data
define spir_func i64 @llpc.readlane.i64(i64 %value, i32 %id)
{
    %value.v2 = bitcast i64 %value to <2 x i32>
    %1 = extractelement <2 x i32> %value.v2, i32 0
    %2 = extractelement <2 x i32> %value.v2, i32 1
    %3 = call i32 @llvm.amdgcn.readlane(i32 %1, i32 %id)
    %4 = call i32 @llvm.amdgcn.readlane(i32 %2, i32 %id)
    %5 = insertelement <2 x i32> undef, i32 %3, i32 0
    %6 = insertelement <2 x i32> %5, i32 %4, i32 1
    %7 = bitcast <2 x i32> %6 to i64

    ret i64 %7
}

; GLSL: int64_t/uint64_t subgroupXXX(int64_t/uint64_t)
define spir_func i64 @llpc.subgroup.reduce.i64(i32 %binaryOp, i64 %value)
{
    ; ds_swizzle work in 32 consecutive lanes/threads BIT mode
    ; log2(64) = 6 , so there are 6 iteration of binary ops needed

    ; 1055 ,bit mode, xor mask = 1 ->(SWAP, 1)
    %i1.1 = call i64 @llpc.swizzle.i64(i64 %value, i32 1055)
    %i1.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i1.1)
    %i1.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %value, i64 %i1.2)

    ; 2079 ,bit mode, xor mask = 2 ->(SWAP, 2)
    %i2.1 = call i64 @llpc.swizzle.i64(i64 %i1.3, i32 2079)
    %i2.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i2.1)
    %i2.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i1.3, i64 %i2.2)

    ; 4127 ,bit mode, xor mask = 4 ->(SWAP, 4)
    %i3.1 = call i64 @llpc.swizzle.i64(i64 %i2.3, i32 4127)
    %i3.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i3.1)
    %i3.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i2.3, i64 %i3.2)

    ; 8223 ,bit mode, xor mask = 8 ->(SWAP, 8)
    %i4.1 = call i64 @llpc.swizzle.i64(i64 %i3.3, i32 8223)
    %i4.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i4.1)
    %i4.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i3.3, i64 %i4.2)

    ; 16415 ,bit mode, xor mask = 16 >(SWAP, 16)
    %i5.1 = call i64 @llpc.swizzle.i64(i64 %i4.3, i32 16415)
    %i5.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i5.1)
    %i5.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i4.3, i64 %i5.2)
    %i5.4 = call i64 @llvm.amdgcn.wwm.i64(i64 %i5.3)

    %i6.1 = call i64 @llpc.readlane.i64(i64 %i5.4, i32 31)
    %i6.2 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i6.1, i64 %i5.4)
    %i6.3 = call i64 @llvm.amdgcn.wwm.i64(i64 %i6.2)
    %i6.4 = call i64 @llpc.readlane.i64(i64 %i6.3, i32 63)

    ret i64 %i6.4
}

; GLSL: int64_t/uint64_t subgroupExclusiveXXX(int64_t/uint64_t)
define spir_func i64 @llpc.subgroup.exclusiveScan.i64(i32 %binaryOp, i64 %value)
{
    %tid.lo =  call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0) #1
    %tid = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %tid.lo) #1
    %tid.64 = zext i32 %tid to i64
    %tmask = shl i64 1, %tid.64
    %tidmask = call i64 @llvm.amdgcn.wwm.i64(i64 %tmask)

    ; ds_swizzle work in 32 consecutive lanes/threads BIT mode
    ; 11 iteration of binary ops needed
    ; 1055, bit mode, xor mask = 1 ->(SWAP, 1)
    %i1.1 = call i64 @llpc.swizzle.i64(i64 %value, i32 1055)
    %i1.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i1.1)
    %i1.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %value, i64 %i1.2)
    ; -6148914691236517206 = 0xAAAA,AAAA,AAAA,AAAA, update lanes/threads according to mask
    %i1.4 = call i64 @llpc.cndmask.i64(i64 %tidmask , i64 -6148914691236517206, i64 %value, i64 %i1.3)
    %i1.5 = call i64 @llvm.amdgcn.wwm.i64(i64 %i1.4)

    ; 2079, bit mode, xor mask = 2 ->(SWAP, 2)
    %i2.1 = call i64 @llpc.swizzle.i64(i64 %i1.5, i32 2079)
    %i2.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i2.1)
    %i2.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i1.4, i64 %i2.2)
    ; -8608480567731124088 = 0x8888,8888,8888,8888
    %i2.4 = call i64 @llpc.cndmask.i64(i64 %tidmask ,i64 -8608480567731124088, i64 %i1.4, i64 %i2.3)

    ; 4127, bit mode, xor mask = 4 ->(SWAP, 4)
    %i3.1 = call i64 @llpc.swizzle.i64(i64 %i2.4, i32 4127)
    %i3.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i3.1)
    %i3.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i2.4, i64 %i3.2)
    ; -9187201950435737472 = 0x8080,8080,8080,8080
    %i3.4 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -9187201950435737472, i64 %i2.4, i64 %i3.3)

    ; 8223, bit mode, xor mask = 8 >(SWAP, 8)
    %i4.1 = call i64 @llpc.swizzle.i64(i64 %i3.4, i32 8223)
    %i4.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i4.1)
    %i4.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i3.4, i64 %i4.2)
    ; -9223231297218904064 = 0x8000,8000,8000,8000
    %i4.4 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -9223231297218904064, i64 %i3.4, i64 %i4.3)

    ; 16415, bit mode, xor mask = 16 >(SWAP, 16)
    %i5.1 = call i64 @llpc.swizzle.i64(i64 %i4.4, i32 16415)
    %i5.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i5.1)
    %i5.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i4.4, i64 %i5.2)
    ; -9223372034707292160 = 0x8000,0000,8000,0000
    %i5.4 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -9223372034707292160, i64 %i4.4, i64 %i5.3)

    ; From now on, scan would be downward
    %identity.i64 = call i64 @llpc.subgroup.identity.i64(i32 %binaryOp)
    %i6.1 = call i64 @llpc.readlane.i64(i64 %i5.4, i32 31)
    %i6.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i6.1)
    ; -9223372036854775808 = 0x8000,0000,0000,0000
    %i6.3 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -9223372036854775808, i64 %i5.4, i64 %i6.2)
    ; 2147483648 = 0x0000,0000,8000,0000
    %i6.4 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 2147483648, i64 %i6.3, i64 %identity.i64)

    ; 16415, bit mode, xor mask = 16 >(SWAP, 16)
    %i7.1 = call i64 @llpc.swizzle.i64(i64 %i6.4, i32 16415)
    %i7.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i7.1)
    ; 140737488388096 = 0x0000,8000,0000,8000
    %i7.3 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 140737488388096, i64 %i6.4, i64 %i7.2)
    %i7.4 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i7.2, i64 %i7.3)
    ; -9223372034707292160 = 0x8000,0000,8000,0000
    %i7.5 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -9223372034707292160, i64 %i7.3, i64 %i7.4)

    ; 8223, bit mode, xor mask = 8 >(SWAP, 8)
    %i8.1 = call i64 @llpc.swizzle.i64(i64 %i7.5, i32 8223)
    %i8.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i8.1)
    ; 36029346783166592 = 0x0080,0080,0080,0080
    %i8.3 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 36029346783166592, i64 %i7.5, i64 %i8.2)
    %i8.4 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i8.2, i64 %i8.3)
    ; -9223231297218904064 = 0x8000,8000,8000,8000
    %i8.5 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -9223231297218904064, i64 %i8.3, i64 %i8.4)

    ; 4127, bit mode, xor mask = 4 ->(SWAP, 4)
    %i9.1 = call i64 @llpc.swizzle.i64(i64 %i8.5, i32 4127)
    %i9.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i9.1)
    ; 578721382704613384 = 0x0808,0808,0808,0808
    %i9.3 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 578721382704613384, i64 %i8.5, i64 %i9.2)
    %i9.4 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i9.2, i64 %i9.3)
    ; -9187201950435737472 = 0x8080,8080,8080,8080
    %i9.5 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -9187201950435737472, i64 %i9.3, i64 %i9.4)

    ; 2079, bit mode, xor mask = 2 ->(SWAP, 2)
    %i10.1 = call i64 @llpc.swizzle.i64(i64 %i9.5, i32 2079)
    %i10.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i10.1)
    ; 2459565876494606882 = 0x2222,2222,2222,2222
    %i10.3 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 2459565876494606882, i64 %i9.5, i64 %i10.2)
    %i10.4 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i10.2, i64 %i10.3)
    ; -8608480567731124088 = 0x8888,8888,8888,8888
    %i10.5 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -8608480567731124088, i64 %i10.3, i64 %i10.4)

    ; 1055, bit mode, xor mask = 1 ->(SWAP, 1)
    %i11.1 = call i64 @llpc.swizzle.i64(i64 %i10.5, i32 1055)
    %i11.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i11.1)
    ; 6148914691236517205 = 0x5555,5555,5555,5555
    %i11.3 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 6148914691236517205, i64 %i10.5, i64 %i11.2)
    %i11.4 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i11.2, i64 %i11.3)
    ; -6148914691236517206 = 0xAAAA,AAAA,AAAA,AAAA
    %i11.5 = call i64 @llpc.cndmask.i64(i64 %tidmask, i64 -6148914691236517206, i64 %i11.3, i64 %i11.4)
    %i11.6 = call i64 @llvm.amdgcn.wwm.i64(i64 %i11.5)

    ret i64 %i11.6
}

; GLSL: int64_t/uint64_t subgroupInclusiveXXX(int64_t/uint64_t)
define spir_func i64 @llpc.subgroup.inclusiveScan.i64(i32 %binaryOp, i64 %value)
{
    %1 = call i64 @llpc.subgroup.exclusiveScan.i64(i32 %binaryOp, i64 %value)
    %2 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %1, i64 %value)

    ret i64 %2
}

declare i64 @llvm.amdgcn.wwm.i64(i64) #1
declare i64 @llvm.amdgcn.set.inactive.i64(i64, i64) #2
declare i32 @llvm.amdgcn.ds.swizzle(i32, i32) #2
declare i32 @llvm.amdgcn.mbcnt.lo(i32, i32) #1
declare i32 @llvm.amdgcn.mbcnt.hi(i32, i32) #1
declare i32 @llvm.amdgcn.readlane(i32, i32) #2
declare i64 @llpc.sminnum.i64(i64, i64) #0
declare i64 @llpc.smaxnum.i64(i64, i64) #0
declare i64 @llpc.uminnum.i64(i64, i64) #0
declare i64 @llpc.umaxnum.i64(i64, i64) #0
declare double @llvm.minnum.f64(double, double) #0
declare double @llvm.maxnum.f64(double, double) #0

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readnone convergent }
attributes #3 = { convergent nounwind }
attributes #4 = { nounwind readonly }
