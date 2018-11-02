;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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

;======================================================================================================================
; QueryNonLod.sizelod implementations:
; Format:    llpc.image.querynonlod.op.dim[Array][.sample].retType
;   dim                 retType
;----------------------------------
;   Buffer              i32
;   1D                  i32
;   2D                  v2i32
;   Cube                v2i32
;   1DArray             v2i32
;   3D                  v3i32
;   2DArray             v3i32
;   CubeArray           v3i32
;   2D.sample           v2i32
;   2DArray.sample      v3i32
;======================================================================================================================

define <2 x i32> @llpc.image.querynonlod.sizelod.1DArray.v2i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    ; NOTE: For 1D array on GFX9, need to use dmask 5 to get array layer as 2D array.
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 5,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 true)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = insertelement <2 x i32> undef, i32 %3, i32 0
    %6 = insertelement <2 x i32> %5, i32 %4, i32 1
    ret <2 x i32> %6
}

define <2 x i32> @llpc.image.querynonlod.sizelod.1DArray.v2i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    ; NOTE: For 1D array on GFX9, need to use dmask 5 to get array layer as 2D array.
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2darray.v4f32.i32(i32 5,
                                                                          i32 %lod,
                                                                          <8 x i32> %resource,
                                                                          i32 0,
                                                                          i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = insertelement <2 x i32> undef, i32 %3, i32 0
    %6 = insertelement <2 x i32> %5, i32 %4, i32 1
    ret <2 x i32> %6
}


declare <8 x i32> @llpc.descriptor.load.resource(i32 , i32 , i32, i32) #0

declare <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 , <8 x i32> , i32, i1, i1, i1, i1) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.2darray.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
