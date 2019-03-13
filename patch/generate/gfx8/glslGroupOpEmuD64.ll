;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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

; Perform update.dpp on 64-bit data
define spir_func i64 @llpc.update.dpp.i64(i64 %identity, i64 %value,i32 %dpp_ctrl, i32 %row_mask, i32 %bank_mask, i1 %bound_ctrl)
{
    %identity.v2 = bitcast i64 %identity to <2 x i32>
    %identity.0 = extractelement <2 x i32> %identity.v2, i32 0
    %identity.1 = extractelement <2 x i32> %identity.v2, i32 1

    %value.v2 = bitcast i64 %value to <2 x i32>
    %value.0 = extractelement <2 x i32> %value.v2, i32 0
    %value.1 = extractelement <2 x i32> %value.v2, i32 1

    %1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity.0, i32 %value.0, i32 %dpp_ctrl, i32 %row_mask, i32 %bank_mask, i1 %bound_ctrl)
    %2 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity.1, i32 %value.1, i32 %dpp_ctrl, i32 %row_mask, i32 %bank_mask, i1 %bound_ctrl)
    %3 = insertelement <2 x i32> undef, i32 %1, i32 0
    %4 = insertelement <2 x i32> %3, i32 %2, i32 1
    %5 = bitcast <2 x i32> %4 to i64

    ret i64 %5
}

; Perform mov.dpp on 64-bit data
define spir_func i64 @llpc.mov.dpp.i64(i64 %value,i32 %dpp_ctrl, i32 %row_mask, i32 %bank_mask, i1 %bound_ctrl)
{
    %value.v2 = bitcast i64 %value to <2 x i32>
    %value.0 = extractelement <2 x i32> %value.v2, i32 0
    %value.1 = extractelement <2 x i32> %value.v2, i32 1

    %1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value.0, i32 %dpp_ctrl, i32 %row_mask, i32 %bank_mask, i1 %bound_ctrl)
    %2 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value.1, i32 %dpp_ctrl, i32 %row_mask, i32 %bank_mask, i1 %bound_ctrl)
    %3 = insertelement <2 x i32> undef, i32 %1, i32 0
    %4 = insertelement <2 x i32> %3, i32 %2, i32 1
    %5 = bitcast <2 x i32> %4 to i64

    ret i64 %5
}

; GLSL: int64_t/uint64_t subgroupInclusiveXXX(int64_t/uint64_t)
define spir_func i64 @llpc.subgroup.inclusiveScan.i64(i32 %binaryOp, i64 %value)
{
    ; dpp_ctrl 273 = 0x111, DPP_ROW_SR, row/lanes shift by 1 thread
    ; Bound control is 0, no write out of bounds for all following dpp mov
    %identity = call i64 @llpc.subgroup.identity.i64(i32 %binaryOp)
    %i1.1 = call i64 @llpc.update.dpp.i64(i64 %identity, i64 %value, i32 273, i32 15, i32 15, i1 false)
    %i1.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i1.1)
    %i1.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %value, i64 %i1.2)

    ; dpp_ctrl 274 = 0x112, DPP_ROW_SR, row/lanes shift by 2 thread
    %i2.1 = call i64 @llpc.update.dpp.i64(i64 %identity, i64 %value, i32 274, i32 15, i32 15, i1 false)
    %i2.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i2.1)
    %i2.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i1.3, i64 %i2.2)

    ; dpp_ctrl 275 = 0x113, DPP_ROW_SR, row/lanes shift by 3 thread
    %i3.1 = call i64 @llpc.update.dpp.i64(i64 %identity, i64 %value, i32 275, i32 15, i32 15, i1 false)
    %i3.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i3.1)
    %i3.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i2.3, i64 %i3.2)

    ; dpp_ctrl 276 = 0x114, DPP_ROW_SR, row/lanes shift by 4 thread
    %i4.1 = call i64 @llpc.update.dpp.i64(i64 %identity, i64 %i3.3, i32 276, i32 15, i32 14, i1 false)
    %i4.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i4.1)
    %i4.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i3.3, i64 %i4.2)

    ; dpp_ctrl 280 = 0x118, DPP_ROW_SR, row/lanes shift by 8 thread
    %i5.1 = call i64 @llpc.update.dpp.i64(i64 %identity, i64 %i4.3, i32 280, i32 15, i32 12, i1 false)
    %i5.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i5.1)
    %i5.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i4.3, i64 %i5.2)

    ; dpp_ctrl 322 =0x142 ,DPP_ROW_BCA, broadcast 15th thread to next row,
    ; row_mask 10 = 0b1010, first and third row broadcast to second and fourth row
    %i6.1 = call i64 @llpc.update.dpp.i64(i64 %identity, i64 %i5.3, i32 322, i32 10, i32 15, i1 false)
    %i6.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i6.1)
    %i6.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i5.3, i64 %i6.2)

    ; dpp_ctrl 323 =0x143 ,DPP_ROW_BCA, broadcast 31th thread to next row 2 and 3,
    ; row_mask 12 = 0b1100, first and second row broadcast to third and fouth row
    %i7.1 = call i64 @llpc.update.dpp.i64(i64 %identity, i64 %i6.3, i32 323, i32 12, i32 15, i1 false)
    %i7.2 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i6.3, i64 %i7.1)
    %i7.3 = call i64 @llvm.amdgcn.wwm.i64(i64 %i7.2)

    ret i64 %i7.3
}

; GLSL: int64_t/uint64_t subgroupXXX(int64_t/uint64_t)
define spir_func i64 @llpc.subgroup.reduce.i64(i32 %binaryOp, i64 %value)
{
    ; dpp_ctrl 273 = 0x111, DPP_ROW_SR, row/lanes shift by 1 thread
    ; Bound control is 0, no write out of bounds for all following dpp mov
    %i1.1 = call i64 @llpc.mov.dpp.i64(i64 %value, i32 273, i32 15, i32 15, i1 false)
    %i1.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i1.1)
    %i1.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %value, i64 %i1.2)

    ; dpp_ctrl 274 = 0x112, DPP_ROW_SR, row/lanes shift by 2 thread
    %i2.1 = call i64 @llpc.mov.dpp.i64(i64 %i1.3, i32 274, i32 15, i32 15, i1 false)
    %i2.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i2.1)
    %i2.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i1.3, i64 %i2.2)

    ; dpp_ctrl 276 = 0x114, DPP_ROW_SR, row/lanes shift by 4 thread
    %i3.1 = call i64 @llpc.mov.dpp.i64(i64 %i2.3, i32 276, i32 15, i32 15, i1 false)
    %i3.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i3.1)
    %i3.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i2.3, i64 %i3.2)

    ; dpp_ctrl 280 = 0x118, DPP_ROW_SR, row/lanes shift by 8 thread
    %i4.1 = call i64 @llpc.mov.dpp.i64(i64 %i3.3, i32 280, i32 15, i32 15, i1 false)
    %i4.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i4.1)
    %i4.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i3.3, i64 %i4.2)

    ; dpp_ctrl 322 =0x142 ,DPP_ROW_BCA, broadcast 15th thread to next row,
    ; row_mask 10 = 0b1010, first and third row broadcast to second and fourth row
    %i5.1 = call i64 @llpc.mov.dpp.i64(i64 %i4.3, i32 322, i32 10, i32 15, i1 false)
    %i5.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i5.1)
    %i5.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i4.3, i64 %i5.2)

    ; dpp_ctrl 323 =0x143 ,DPP_ROW_BCA, broadcast 31th thread to next row 2 and 3,
    ; row_mask 12 = 0b1100, first and second row broadcast to third and fouth row
    %i6.1 = call i64 @llpc.mov.dpp.i64(i64 %i5.3, i32 323, i32 12, i32 15, i1 false)
    %i6.2 = call i64 @llvm.amdgcn.wwm.i64(i64 %i6.1)
    %i6.3 = call i64 @llpc.subgroup.arithmetic.i64(i32 %binaryOp, i64 %i5.3, i64 %i6.2)
    %i6.4 = call i64 @llpc.readlane.i64(i64 %i6.3, i32 63)
    %i6.5 = call i64 @llvm.amdgcn.wwm.i64(i64 %i6.4)

    ret i64 %i6.5
}

; GLSL: int64_t/uint64_t subgroupExclusiveXXX(int64_t/uint64_t)
define spir_func i64 @llpc.subgroup.exclusiveScan.i64(i32 %binaryOp, i64 %value)
{
    %identity = call i64 @llpc.subgroup.identity.i64(i32 %binaryOp)
    ; dpp_ctrl 312 =0x138, DPP_WF_SR1, wavefront right shift by 1 threads
    ; bound ctrl is true, always write even out of bounds.
    %1 = call i64 @llpc.update.dpp.i64(i64 %identity, i64 %value, i32 312, i32 15, i32 15, i1 false)
    %2 = call i64 @llpc.subgroup.inclusiveScan.i64(i32 %binaryOp, i64 %1)

    ret i64 %2
}

declare i32 @llvm.amdgcn.update.dpp.i32(i32, i32, i32, i32, i32, i1) #2
declare i32 @llvm.amdgcn.mov.dpp.i32(i32, i32, i32, i32, i1) #2
declare spir_func i64 @llpc.subgroup.arithmetic.i64(i32, i64, i64 ) #0
declare spir_func i64 @llpc.readlane.i64(i64, i32) #0
declare spir_func i64 @llpc.subgroup.identity.i64(i32) #0

declare i64 @llvm.amdgcn.wwm.i64(i64) #1
declare <2 x i64> @llvm.amdgcn.wwm.v2i64(<2 x i64>) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readnone convergent }
