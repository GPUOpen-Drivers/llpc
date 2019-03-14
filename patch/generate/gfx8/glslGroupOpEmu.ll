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
define spir_func i32 @llpc.subgroup.inclusiveScan.i32(i32 %binaryOp, i32 %value)
{
    ; dpp_ctrl 273 = 0x111, DPP_ROW_SR, row/lanes shift by 1 thread
    ; Bound control is 0, no write out of bounds for all following dpp mov
    %identity = call i32 @llpc.subgroup.identity.i32(i32 %binaryOp)
    %i1.1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity, i32 %value, i32 273, i32 15, i32 15, i1 false)
    %i1.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i1.1)
    %i1.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %value, i32 %i1.2)

    ; dpp_ctrl 274 = 0x112, DPP_ROW_SR, row/lanes shift by 2 thread
    %i2.1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity, i32 %value, i32 274, i32 15, i32 15, i1 false)
    %i2.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i2.1)
    %i2.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i1.3, i32 %i2.2)

    ; dpp_ctrl 275 = 0x113, DPP_ROW_SR, row/lanes shift by 3 thread
    %i3.1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity, i32 %value, i32 275, i32 15, i32 15, i1 false)
    %i3.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i3.1)
    %i3.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i2.3, i32 %i3.2)

    ; dpp_ctrl 276 = 0x114, DPP_ROW_SR, row/lanes shift by 4 thread
    %i4.1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity, i32 %i3.3, i32 276, i32 15, i32 14, i1 false)
    %i4.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i4.1)
    %i4.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i3.3, i32 %i4.2)

    ; dpp_ctrl 280 = 0x118, DPP_ROW_SR, row/lanes shift by 8 thread
    %i5.1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity, i32 %i4.3, i32 280, i32 15, i32 12, i1 false)
    %i5.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i5.1)
    %i5.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i4.3, i32 %i5.2)

    ; dpp_ctrl 322 =0x142 ,DPP_ROW_BCA, broadcast 15th thread to next row,
    ; row_mask 10 = 0b1010, first and third row broadcast to second and fourth row
    %i6.1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity, i32 %i5.3, i32 322, i32 10, i32 15, i1 false)
    %i6.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i6.1)
    %i6.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i5.3, i32 %i6.2)

    ; dpp_ctrl 323 =0x143 ,DPP_ROW_BCA, broadcast 31th thread to next row 2 and 3,
    ; row_mask 12 = 0b1100, first and second row broadcast to third and fouth row
    %i7.1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity, i32 %i6.3, i32 323, i32 12, i32 15, i1 false)
    %i7.2 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i6.3, i32 %i7.1)
    %i7.3 = call i32 @llvm.amdgcn.wwm.i32(i32 %i7.2)

    ret i32 %i7.3
}

; GLSL: int/uint/float subgroupXXX(int/uint/float)
define spir_func i32 @llpc.subgroup.reduce.i32(i32 %binaryOp, i32 %value)
{
    ; dpp_ctrl 273 = 0x111, DPP_ROW_SR, row/lanes shift by 1 thread
    ; Bound control is 0, no write out of bounds for all following dpp mov
    %i1.1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value, i32 273, i32 15, i32 15, i1 false)
    %i1.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i1.1)
    %i1.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %value, i32 %i1.2)

    ; dpp_ctrl 274 = 0x112, DPP_ROW_SR, row/lanes shift by 2 thread
    %i2.1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %i1.3, i32 274, i32 15, i32 15, i1 false)
    %i2.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i2.1)
    %i2.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i1.3, i32 %i2.2)

    ; dpp_ctrl 276 = 0x114, DPP_ROW_SR, row/lanes shift by 4 thread
    %i3.1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %i2.3, i32 276, i32 15, i32 15, i1 false)
    %i3.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i3.1)
    %i3.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i2.3, i32 %i3.2)

    ; dpp_ctrl 280 = 0x118, DPP_ROW_SR, row/lanes shift by 8 thread
    %i4.1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %i3.3, i32 280, i32 15, i32 15, i1 false)
    %i4.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i4.1)
    %i4.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i3.3, i32 %i4.2)

    ; dpp_ctrl 322 =0x142 ,DPP_ROW_BCA, broadcast 15th thread to next row,
    ; row_mask 10 = 0b1010, first and third row broadcast to second and fourth row
    %i5.1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %i4.3, i32 322, i32 10, i32 15, i1 false)
    %i5.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i5.1)
    %i5.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i4.3, i32 %i5.2)

    ; dpp_ctrl 323 =0x143 ,DPP_ROW_BCA, broadcast 31th thread to next row 2 and 3,
    ; row_mask 12 = 0b1100, first and second row broadcast to third and fouth row
    %i6.1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %i5.3, i32 323, i32 12, i32 15, i1 false)
    %i6.2 = call i32 @llvm.amdgcn.wwm.i32(i32 %i6.1)
    %i6.3 = call i32 @llpc.subgroup.arithmetic.i32(i32 %binaryOp, i32 %i5.3, i32 %i6.2)
    %i6.4 = call i32 @llvm.amdgcn.readlane(i32 %i6.3, i32 63)
    %i6.5 = call i32 @llvm.amdgcn.wwm.i32(i32 %i6.4)

    ret i32 %i6.5
}

; GLSL: int/uint/float subgroupExclusiveXXX(int/uint/float)
define spir_func i32 @llpc.subgroup.exclusiveScan.i32(i32 %binaryOp, i32 %value)
{
    %identity = call i32 @llpc.subgroup.identity.i32(i32 %binaryOp)
    ; dpp_ctrl 312 =0x138, DPP_WF_SR1, wavefront right shift by 1 threads
    ; bound ctrl is false, no write out of bounds
    %1 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %identity, i32 %value, i32 312, i32 15, i32 15, i1 false)
    %2 = call i32 @llpc.subgroup.inclusiveScan.i32(i32 %binaryOp, i32 %1)

    ret i32 %2
}

; GLSL: int/uint subgroupShuffle(int/uint, uint)
define spir_func i32 @_Z22GroupNonUniformShuffleiii(i32 %scope, i32 %value, i32 %id)
{
    %1 = mul i32 %id, 4
    %2 = call i32 @llvm.amdgcn.ds.bpermute(i32 %1, i32 %value)

    ret i32 %2
}

; GLSL: int/uint subgroupQuadBroadcast(int/uint, uint)
define spir_func i32 @_Z28GroupNonUniformQuadBroadcastiii(i32 %scope, i32 %value, i32 %id)
{
    ; id should be constant of 0 ~ 3
.entry:
    switch i32 %id, label %.end [ i32 0, label %.id0
                                  i32 1, label %.id1
                                  i32 2, label %.id2
                                  i32 3, label %.id3 ]
.id0:
    ; QUAD_PERM 0,0,0,0
    %value.dpp0 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value, i32 0, i32 15, i32 15, i1 true)
    br label %.end
.id1:
    ; QUAD_PERM 1,1,1,1
    %value.dpp1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value, i32 85, i32 15, i32 15, i1 true)
    br label %.end
.id2:
    ; QUAD_PERM 2,2,2,2
    %value.dpp2 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value, i32 170, i32 15, i32 15, i1 true)
    br label %.end
.id3:
    ; QUAD_PERM 3,3,3,3
    %value.dpp3 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value, i32 255, i32 15, i32 15, i1 true)
    br label %.end
.end:
    %result = phi i32 [undef, %.entry],[%value.dpp0, %.id0],[%value.dpp1, %.id1], [%value.dpp2, %.id2], [%value.dpp3, %.id3]
    ret i32 %result
}

; GLSL: int/uint subgroupQuadSwapHorizontal(int/uint)
;       int/uint subgroupQuadSwapVertical(int/uint)
;       int/uint subgroupQuadSwapDiagonal(int/uint)
define spir_func i32 @_Z23GroupNonUniformQuadSwapiii(i32 %scope, i32 %value, i32 %direction)
{
    ; direction 0 is Horizontal
    ; direction 1 is Vertical
    ; direction 2 is Diagonal
.entry:
    switch i32 %direction, label %.end [ i32 0, label %.horizonal
                                         i32 1, label %.vertical
                                         i32 2, label %.diagonal ]
.horizonal:
    ; QUAD_PERM [ 0->1, 1->0, 2->3, 3->2], 0b1011,0001
    %value.dir0 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value, i32 177, i32 15, i32 15, i1 true)
    br label %.end
.vertical:
    ; QUAD_PERM [ 0->2, 1->3, 2->0, 3->1], 0b0100,1110
    %value.dir1 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value, i32 78, i32 15, i32 15, i1 true)
    br label %.end
.diagonal:
    ; QUAD_PERM [ 0->3, 1->2, 2->1, 3->0], 0b0001,1011
    %value.dir2 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %value, i32 27, i32 15, i32 15, i1 true)
    br label %.end
.end:
    %result = phi i32 [undef, %.entry], [%value.dir0, %.horizonal], [%value.dir1, %.vertical], [%value.dir2, %.diagonal]
    ret i32 %result
}

; GLSL: int/uint swizzleInvocations(int/uint, uvec4)
define spir_func i32 @_Z21SwizzleInvocationsAMDiDv4_i(i32 %data, <4 x i32> %offset)
{
    %1 = extractelement <4 x i32> %offset, i32 0
    %2 = extractelement <4 x i32> %offset, i32 1
    %3 = extractelement <4 x i32> %offset, i32 2
    %4 = extractelement <4 x i32> %offset, i32 3

    %5 = and i32 %1, 3
    %6 = and i32 %2, 3
    %7 = and i32 %3, 3
    %8 = and i32 %4, 3

    ; [7:6] = offset[3], [5:4] = offset[2], [3:2] = offset[1], [1:0] = offset[0]
    %9  = shl i32 %6, 2
    %10 = shl i32 %7, 4
    %11 = shl i32 %8, 6

    %12 = or i32 %5,  %9
    %13 = or i32 %12, %10
    %14 = or i32 %13, %11

    ; row_mask = 0xF, bank_mask = 0xF, bound_ctrl = true
    %15 = call i32 @llvm.amdgcn.mov.dpp.i32(i32 %data, i32 %14, i32 15, i32 15, i1 true)

    ret i32 %15
}

declare i32 @llvm.amdgcn.mov.dpp.i32(i32, i32, i32, i32, i1) #2
declare i32 @llvm.amdgcn.wwm.i32(i32) #1
declare i32 @llvm.amdgcn.update.dpp.i32(i32, i32, i32, i32, i32, i1) #2
declare i32 @llvm.amdgcn.wqm.i32(i32) #1
declare i32 @llvm.amdgcn.ds.bpermute(i32, i32) #2
declare i32 @llvm.amdgcn.readlane(i32, i32) #2
declare i32 @llvm.amdgcn.readfirstlane(i32) #2
declare spir_func i32 @llpc.subgroup.arithmetic.i32(i32, i32 , i32) #0
declare spir_func i32 @llpc.subgroup.identity.i32(i32) #0

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readnone convergent }

