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

; GLSL: identity (16-bit)
define spir_func i32 @llpc.subgroup.identity.i16(i32 %binaryOp)
{
.entry:
    switch i32 %binaryOp, label %.end [i32 0,  label %.iadd
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
    ret i32 0
.imul:
    ret i32 1
.smin:
    ; 0x7FFF
    ret i32 32767
.smax:
    ; FFFF8000
    ret i32 4294934528
.umin:
    ; 0xFFFF FFFF
    ret i32 4294967295
.umax:
    ret i32 0
.and:
    ; 0xFFFF FFFF
    ret i32 4294967295
.or:
    ret i32 0
.xor:
    ret i32 0
.fmul:
    ; 0x3F800000, 1.0
    ret i32 1065353216
.fmin:
    ; 0x7F800000, +1.#INF00E+000
    ret i32 2139095040
.fmax:
    ; 0xFF800000  -1.#INF00E+000
    ret i32 4286578688
.fadd:
    ret i32 0
.end:
    ret i32 0
}

; GLSL: x [binary] y (16-bit)
define spir_func i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %x, i32 %y)
{
.entry:
    switch i32 %binaryOp, label %.end [i32 0,  label %.iadd
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
    %0 = add i32 %x, %y
    br label %.end
.imul:
    %1 = mul i32 %x, %y
    br label %.end
.smin:
    %2 = call i32 @llpc.sminnum.i32(i32 %x, i32 %y)
    br label %.end
.smax:
    ; smax must act as 16-bit arithmetic operation
    %s.0 = trunc i32 %x to i16
    %s.1 = trunc i32 %y to i16
    %s.2 = icmp slt i16 %s.0, %s.1
    %s.3 = select i1 %s.2, i16 %s.1, i16 %s.0
    %3 = sext i16 %s.3 to i32
    br label %.end
.umin:
    %4 = call i32 @llpc.uminnum.i32(i32 %x, i32 %y)
    br label %.end
.umax:
    %5 = call i32 @llpc.umaxnum.i32(i32 %x, i32 %y)
    br label %.end
.and:
    %6 = and i32 %x, %y
    br label %.end
.or:
    %7 = or i32 %x, %y
    br label %.end
.xor:
    %8 = xor i32 %x, %y
    br label %.end
.fmul:
    %x.fmul.f32 = bitcast i32 %x to float
    %y.fmul.f32 = bitcast i32 %y to float
    %fmul.f32 = fmul float %x.fmul.f32, %y.fmul.f32
    %9 = bitcast float %fmul.f32 to i32
    br label %.end
.fmin:
    %x.fmin.f32 = bitcast i32 %x to float
    %y.fmin.f32 = bitcast i32 %y to float
    %fmin.f32 = call float @llvm.minnum.f32(float %x.fmin.f32, float %y.fmin.f32)
    %10 = bitcast float %fmin.f32 to i32
    br label %.end
.fmax:
    %x.fmax.f32 = bitcast i32 %x to float
    %y.fmax.f32 = bitcast i32 %y to float
    %fmax.f32 = call float @llvm.maxnum.f32(float %x.fmax.f32, float %y.fmax.f32)
    %11 = bitcast float %fmax.f32 to i32
    br label %.end
.fadd:
    %x.fadd.f32 = bitcast i32 %x to float
    %y.fadd.f32 = bitcast i32 %y to float
    %fadd.f32 = fadd float %x.fadd.f32, %y.fadd.f32
    %12 = bitcast float %fadd.f32 to i32
    br label %.end
.end:
    %result = phi i32 [undef, %.entry], [%0, %.iadd], [%1, %.imul], [%2, %.smin],
              [%3, %.smax], [%4, %.umin], [%5, %.umax], [%6, %.and], [%7, %.or],
              [%8, %.xor], [%9, %.fmul], [%10, %.fmin], [%11, %.fmax], [%12, %.fadd]
    ret i32 %result
}

; Set values to all inactive lanes (16 bit)
define spir_func i32 @llpc.subgroup.set.inactive.i16(i32 %binaryOp, i32 %value)
{
    ; Get identity value of binary operations
    %identity = call i32 @llpc.subgroup.identity.i16(i32 %binaryOp)
    ; Prevent optimization of backend compiler on the control flow
    %1 = call i32 asm sideeffect "; %1", "=v,0"(i32 %value)
    ; Set identity value for the inactive threads
    %activeValue = call i32 @llvm.amdgcn.set.inactive.i32(i32 %1, i32 %identity)

    ret i32 %activeValue
}

; GLSL: int16_t/uint16_t/float16_t subgroupXXX(int16_t/uint16_t/float16_t)
define spir_func i32 @llpc.subgroup.reduce.i16(i32 %binaryOp, i32 %value)
{
    %1 = call i32 @llpc.subgroup.reduce.i32(i32 %binaryOp, i32 %value)
    ret i32 %1
}

; GLSL: int16_t/uint16_t/float16_t subgroupExclusiveXXX(int16_t/uint16_t/float16_t)
define spir_func i32 @llpc.subgroup.exclusiveScan.i16(i32 %binaryOp, i32 %value)
{
    %tid.lo =  call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0) #1
    %tid = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %tid.lo) #1
    %tid.64 = zext i32 %tid to i64
    %tmask = shl i64 1, %tid.64
    %tidmask = call i64 @llvm.amdgcn.wwm.i64(i64 %tmask)

    ; ds_swizzle work in 32 consecutive lanes/threads BIT modeg
    ; 11 iteration of binary ops needed
    ; 1055, bit mode, xor mask = 1 ->(SWAP, 1)
    %i1.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %value, i32 1055)
    %i1.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i1.1)
    %i1.3 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %value, i32 %i1.2)
    ; -6148914691236517206 = 0xAAAA,AAAA,AAAA,AAAA, update lanes/threads according to mask
    %i1.4 = call i32 @llpc.cndmask.i32(i64 %tidmask , i64 -6148914691236517206, i32 %value, i32 %i1.3)
    %i1.5 = call i32 @llvm.amdgcn.wwm.i32(i32 %i1.4)

    ; 2079, bit mode, xor mask = 2 ->(SWAP, 2)
    %i2.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i1.5, i32 2079)
    %i2.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i2.1)
    %i2.3 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i1.4, i32 %i2.2)
    ; -8608480567731124088 = 0x8888,8888,8888,8888
    %i2.4 = call i32 @llpc.cndmask.i32(i64 %tidmask ,i64 -8608480567731124088, i32 %i1.4, i32 %i2.3)

    ; 4127, bit mode, xor mask = 4 ->(SWAP, 4)
    %i3.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i2.4, i32 4127)
    %i3.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i3.1)
    %i3.3 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i2.4, i32 %i3.2)
    ; -9187201950435737472 = 0x8080,8080,8080,8080
    %i3.4 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -9187201950435737472, i32 %i2.4, i32 %i3.3)

    ; 8223, bit mode, xor mask = 8 >(SWAP, 8)
    %i4.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i3.4, i32 8223)
    %i4.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i4.1)
    %i4.3 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i3.4, i32 %i4.2)
    ; -9223231297218904064 = 0x8000,8000,8000,8000
    %i4.4 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -9223231297218904064, i32 %i3.4, i32 %i4.3)

    ; 16415, bit mode, xor mask = 16 >(SWAP, 16)
    %i5.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i4.4, i32 16415)
    %i5.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i5.1)
    %i5.3 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i4.4, i32 %i5.2)
    ; -9223372034707292160 = 0x8000,0000,8000,0000
    %i5.4 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -9223372034707292160, i32 %i4.4, i32 %i5.3)

    ; From now on, scan would be downward
    %identity = call i32 @llpc.subgroup.identity.i16(i32 %binaryOp)
    %i6.1 = call i32 @llvm.amdgcn.readlane(i32 %i5.4, i32 31)
    %i6.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i6.1)
    ; -9223372036854775808 = 0x8000,0000,0000,0000
    %i6.3 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -9223372036854775808, i32 %i5.4, i32 %i6.2)
    ; 2147483648 = 0x0000,0000,8000,0000
    %i6.4 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 2147483648, i32 %i6.3, i32 %identity)

    ; 16415, bit mode, xor mask = 16 >(SWAP, 16)
    %i7.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i6.4, i32 16415)
    %i7.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i7.1)
    ; 140737488388096 = 0x0000,8000,0000,8000
    %i7.3 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 140737488388096, i32 %i6.4, i32 %i7.2)
    %i7.4 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i7.2, i32 %i7.3)
    ; -9223372034707292160 = 0x8000,0000,8000,0000
    %i7.5 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -9223372034707292160, i32 %i7.3, i32 %i7.4)

    ; 8223, bit mode, xor mask = 8 >(SWAP, 8)
    %i8.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i7.5, i32 8223)
    %i8.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i8.1)
    ; 36029346783166592 = 0x0080,0080,0080,0080
    %i8.3 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 36029346783166592, i32 %i7.5, i32 %i8.2)
    %i8.4 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i8.2, i32 %i8.3)
    ; -9223231297218904064 = 0x8000,8000,8000,8000
    %i8.5 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -9223231297218904064, i32 %i8.3, i32 %i8.4)

    ; 4127, bit mode, xor mask = 4 ->(SWAP, 4)
    %i9.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i8.5, i32 4127)
    %i9.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i9.1)
    ; 578721382704613384 = 0x0808,0808,0808,0808
    %i9.3 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 578721382704613384, i32 %i8.5, i32 %i9.2)
    %i9.4 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i9.2, i32 %i9.3)
    ; -9187201950435737472 = 0x8080,8080,8080,8080
    %i9.5 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -9187201950435737472, i32 %i9.3, i32 %i9.4)

    ; 2079, bit mode, xor mask = 2 ->(SWAP, 2)
    %i10.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i9.5, i32 2079)
    %i10.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i10.1)
    ; 2459565876494606882 = 0x2222,2222,2222,2222
    %i10.3 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 2459565876494606882, i32 %i9.5, i32 %i10.2)
    %i10.4 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i10.2, i32 %i10.3)
    ; -8608480567731124088 = 0x8888,8888,8888,8888
    %i10.5 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -8608480567731124088, i32 %i10.3, i32 %i10.4)

    ; 1055, bit mode, xor mask = 1 ->(SWAP, 1)
    %i11.1 = call i32 @llvm.amdgcn.ds.swizzle(i32 %i10.5, i32 1055)
    %i11.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i11.1)
    ; 6148914691236517205 = 0x5555,5555,5555,5555
    %i11.3 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 6148914691236517205, i32 %i10.5, i32 %i11.2)
    %i11.4 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %i11.2, i32 %i11.3)
    ; -6148914691236517206 = 0xAAAA,AAAA,AAAA,AAAA
    %i11.5 = call i32 @llpc.cndmask.i32(i64 %tidmask, i64 -6148914691236517206, i32 %i11.3, i32 %i11.4)
    %i11.6 = call i32 @llvm.amdgcn.wwm.i32(i32 %i11.5)

    ret i32 %i11.6
}

; GLSL: int16_t/uint16_t/float16_t subgroupInclusiveXXX(int16_t/uint16_t/float16_t)
define spir_func i32 @llpc.subgroup.inclusiveScan.i16(i32 %binaryOp, i32 %value)
{
    %1 = call i32 @llpc.subgroup.exclusiveScan.i16(i32 %binaryOp, i32 %value)
    %2 = call i32 @llpc.subgroup.arithmetic.i16(i32 %binaryOp, i32 %1, i32 %value)

    ret i32 %2
}

declare half @llvm.fabs.f16(half) #0
declare i32 @llvm.amdgcn.wqm.i32(i32) #1
declare i32 @llvm.amdgcn.ds.swizzle(i32, i32) #2
declare i32 @llvm.amdgcn.readlane(i32, i32) #2
declare i32 @llvm.amdgcn.mbcnt.lo(i32, i32) #1
declare i32 @llvm.amdgcn.mbcnt.hi(i32, i32) #1
declare i32 @llvm.amdgcn.set.inactive.i32(i32, i32) #2
declare i64 @llvm.amdgcn.wwm.i64(i64) #1
declare i32 @llvm.amdgcn.wwm.i32(i32) #1
declare i32 @llpc.subgroup.reduce.i32(i32 %binaryOp, i32 %value) #0
declare i32 @llpc.cndmask.i32(i64, i64, i32, i32)
declare i32 @llpc.sminnum.i32(i32, i32) #0
declare i32 @llpc.uminnum.i32(i32, i32) #0
declare i32 @llpc.umaxnum.i32(i32, i32) #0
declare float @llvm.minnum.f32(float, float) #0
declare float @llvm.maxnum.f32(float, float) #0

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone}
attributes #2 = { nounwind readnone convergent }
attributes #3 = { convergent nounwind }
attributes #4 = { nounwind readonly }