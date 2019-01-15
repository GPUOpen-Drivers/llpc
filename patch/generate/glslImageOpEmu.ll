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

define i32 @llpc.image.querynonlod.sizelod.1D.i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.1d.v4f32.i32(i32 15,
                                                                     i32 %lod,
                                                                     <8 x i32> %resource,
                                                                     i32 0,
                                                                     i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    ret i32 %3
}

define <2 x i32> @llpc.image.querynonlod.sizelod.2D.v2i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2d.v4f32.i32(i32 15,
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

define <2 x i32> @llpc.image.querynonlod.sizelod.Cube.v2i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.cube.v4f32.i32(i32 15,
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

define <2 x i32> @llpc.image.querynonlod.sizelod.1DArray.v2i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.1darray.v4f32.i32(i32 15,
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

define <3 x i32> @llpc.image.querynonlod.sizelod.3D.v3i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.3d.v4f32.i32(i32 15,
                                                                     i32 %lod,
                                                                     <8 x i32> %resource,
                                                                     i32 0,
                                                                     i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = extractelement <4 x i32> %2, i32 2
    %6 = insertelement <3 x i32> undef, i32 %3, i32 0
    %7 = insertelement <3 x i32> %6, i32 %4, i32 1
    %8 = insertelement <3 x i32> %7, i32 %5, i32 2
    ret <3 x i32> %8
}

define <3 x i32> @llpc.image.querynonlod.sizelod.2DArray.v3i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2darray.v4f32.i32(i32 15,
                                                                          i32 %lod,
                                                                          <8 x i32> %resource,
                                                                          i32 0,
                                                                          i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = extractelement <4 x i32> %2, i32 2
    %6 = insertelement <3 x i32> undef, i32 %3, i32 0
    %7 = insertelement <3 x i32> %6, i32 %4, i32 1
    %8 = insertelement <3 x i32> %7, i32 %5, i32 2
    ret <3 x i32> %8
}

define <3 x i32> @llpc.image.querynonlod.sizelod.CubeArray.v3i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.cube.v4f32.i32(i32 15,
                                                                       i32 %lod,
                                                                       <8 x i32> %resource,
                                                                       i32 0,
                                                                       i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = extractelement <4 x i32> %2, i32 2
    %6 = sdiv i32 %5, 6
    %7 = insertelement <3 x i32> undef, i32 %3, i32 0
    %8 = insertelement <3 x i32> %7, i32 %4, i32 1
    %9 = insertelement <3 x i32> %8, i32 %6, i32 2
    ret <3 x i32> %9
}

define <2 x i32> @llpc.image.querynonlod.sizelod.2D.sample.v2i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2dmsaa.v4f32.i32(i32 15,
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

define <3 x i32> @llpc.image.querynonlod.sizelod.2DArray.sample.v3i32.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2darraymsaa.v4f32.i32(i32 15,
                                                                              i32 %lod,
                                                                              <8 x i32> %resource,
                                                                              i32 0,
                                                                              i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = extractelement <4 x i32> %2, i32 2
    %6 = insertelement <3 x i32> undef, i32 %3, i32 0
    %7 = insertelement <3 x i32> %6, i32 %4, i32 1
    %8 = insertelement <3 x i32> %7, i32 %5, i32 2
    ret <3 x i32> %8
}

define i32 @llpc.image.querynonlod.sizelod.Buffer.i32.gfx6(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <4 x i32> @llpc.descriptor.load.texelbuffer(i32 %resourceDescSet,
                                                                 i32 %resourceBinding,
                                                                 i32 %resourceIdx,
                                                                 i32 %imageCallMeta)

    ; Extract NUM_RECORDS (SQ_BUF_RSRC_WORD2)
    %1 = extractelement <4 x i32> %resource, i32 2

    ret i32 %1
}

define i32 @llpc.image.querynonlod.sizelod.Buffer.i32.gfx8(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <4 x i32> @llpc.descriptor.load.texelbuffer(i32 %resourceDescSet,
                                                                 i32 %resourceBinding,
                                                                 i32 %resourceIdx,
                                                                 i32 %imageCallMeta)
    ; Extract NUM_RECORDS (SQ_BUF_RSRC_WORD2)
    %1 = extractelement <4 x i32> %resource, i32 2

    ; Extract STRIDE (SQ_BUF_RSRC_WORD1, [29:16])
    %2 = extractelement <4 x i32> %resource, i32 1
    %3 = call i32 @llvm.amdgcn.ubfe.i32(i32 %2, i32 16, i32 14)

    ; Buffer size = NUM_RECORDS / STRIDE
    %4 = udiv i32 %1, %3

    ret i32 %4
}

;======================================================================================================================
; QueryNonLod.levels implementations:
; Format:    llpc.image.querynonlod.levels.dim
;   dim
;----------------------------------
;   1D
;   2D
;   Cube
;   1DArray
;   3D
;   2DArray
;   CubeArray
;   2D.sample
;   2DArray.sample
;======================================================================================================================

define i32 @llpc.image.querynonlod.levels.1D.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.1d.v4f32.i32(i32 15,
                                                                     i32 undef,
                                                                     <8 x i32> %resource,
                                                                     i32 0,
                                                                     i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels.2D.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2d.v4f32.i32(i32 15,
                                                                     i32 undef,
                                                                     <8 x i32> %resource,
                                                                     i32 0,
                                                                     i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels.Cube.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.cube.v4f32.i32(i32 15,
                                                                       i32 undef,
                                                                       <8 x i32> %resource,
                                                                       i32 0,
                                                                       i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels.1DArray.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.1darray.v4f32.i32(i32 15,
                                                                          i32 undef,
                                                                          <8 x i32> %resource,
                                                                          i32 0,
                                                                          i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels.3D.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.3d.v4f32.i32(i32 15,
                                                                     i32 undef,
                                                                     <8 x i32> %resource,
                                                                     i32 0,
                                                                     i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels.2DArray.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2darray.v4f32.i32(i32 15,
                                                                          i32 undef,
                                                                          <8 x i32> %resource,
                                                                          i32 0,
                                                                          i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels.CubeArray.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.cube.v4f32.i32(i32 15,
                                                                       i32 undef,
                                                                       <8 x i32> %resource,
                                                                       i32 0,
                                                                       i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels.2D.sample.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2dmsaa.v4f32.i32(i32 15,
                                                                         i32 undef,
                                                                         <8 x i32> %resource,
                                                                         i32 0,
                                                                         i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels.2DArray.sample.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2darraymsaa.v4f32.i32(i32 15,
                                                                              i32 undef,
                                                                              <8 x i32> %resource,
                                                                              i32 0,
                                                                              i32 0)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.levels(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 undef,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 3
    ret i32 %3
}

define i32 @llpc.image.querynonlod.samples(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = extractelement <8 x i32> %resource, i32 3

    ; Extract LAST_LEVEL (SQ_IMG_RSRC_WORD3, [19:16])
    %2 = call i32 @llvm.amdgcn.ubfe.i32(i32 %1, i32 16, i32 4)
    ; Sample numer = 1 << LAST_LEVEL (LAST_LEVEL = log2(sample numer))
    %3 = shl i32 1, %2

    ; Extract TYPE(SQ_IMG_RSRC_WORD3, [31:28])
    %4 = call i32 @llvm.amdgcn.ubfe.i32(i32 %1, i32 28, i32 4)

    ; Check if resource type is 2D MSAA or 2D MSAA array, 14 = SQ_RSRC_IMG_2D_MSAA, 15 = SQ_RSRC_IMG_2D_MSAA_ARRAY
    %5 = icmp eq i32 %4, 14
    %6 = icmp eq i32 %4, 15
    %7 = or i1 %5, %6

    ; Return sample number if resource type is 2D MSAA or 2D MSAA array. Otherwise, return 1.
    %8 = select i1 %7, i32 %3, i32 1

    ret i32 %8
}

define i32 @llpc.image.querynonlod.samples.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = extractelement <8 x i32> %resource, i32 3

    ; Extract LAST_LEVEL (SQ_IMG_RSRC_WORD3, [19:16])
    %2 = call i32 @llvm.amdgcn.ubfe.i32(i32 %1, i32 16, i32 4)
    ; Sample numer = 1 << LAST_LEVEL (LAST_LEVEL = log2(sample numer))
    %3 = shl i32 1, %2

    ; Extract TYPE(SQ_IMG_RSRC_WORD3, [31:28])
    %4 = call i32 @llvm.amdgcn.ubfe.i32(i32 %1, i32 28, i32 4)

    ; Check if resource type is 2D MSAA or 2D MSAA array, 14 = SQ_RSRC_IMG_2D_MSAA, 15 = SQ_RSRC_IMG_2D_MSAA_ARRAY
    %5 = icmp eq i32 %4, 14
    %6 = icmp eq i32 %4, 15
    %7 = or i1 %5, %6

    ; Return sample number if resource type is 2D MSAA or 2D MSAA array. Otherwise, return 1.
    %8 = select i1 %7, i32 %3, i32 1

    ret i32 %8
}

define <8 x i32> @llpc.patch.image.readwriteatomic.descriptor.cube(
    <8 x i32> %resource) #0
{
    ; Modify DEPTH
    %1 = extractelement <8 x i32> %resource, i32 4
    ; Extract DEPTH in resource descriptor
    %2 = call i32 @llvm.amdgcn.ubfe.i32(i32 %1, i32 0, i32 13)
    %3 = mul i32 %2, 6
    %4 = add i32 %3, 5
    ; -8192 = 0xFFFFE000
    %5 = and i32 %1, -8192
    %6 = or i32 %5, %4

    ; Change resource type to 2D array (0xD)
    %7 = extractelement <8 x i32> %resource, i32 3
    ; 268435455 = 0x0FFFFFFF
    %8 = and i32 %7, 268435455
    ; 3489660928 = 0xD0000000
    %9 = or i32 %8, 3489660928

    ; Insert modified value
    %10 = insertelement <8 x i32> %resource, i32 %6, i32 4
    %11 = insertelement <8 x i32> %10, i32 %9, i32 3

    ret <8 x i32> %11
}

define i1 @llpc.patch.image.gather.check(
    <8 x i32> %resource) #0
{
    ; Check whether we have to patch resource descriptor for image gather by checking the data format
    ; of resource descriptor.
    %1 = extractelement <8 x i32> %resource, i32 1
    ; Extract DATA_FORMAT
    %2 = call i32 @llvm.amdgcn.ubfe.i32(i32 %1, i32 20, i32 6)

    %3 = icmp eq i32 %2, 4
    %4 = icmp eq i32 %2, 11
    %5 = icmp eq i32 %2, 14
    %6 = or i1 %4, %3
    %7 = or i1 %5, %6
    %8 = xor i1 %7, 1
    ret i1 %8
}

define <2 x float> @llpc.patch.image.gather.coordinate.2d(
    <8 x i32> %resource, float %x, float %y) #0
{
    ; Get image width and height
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2d.v4f32.i32(i32 15,
                                                                     i32 0,
                                                                     <8 x i32> %resource,
                                                                     i32 0,
                                                                     i32 0)

    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = shufflevector  <4 x i32> %2, <4 x i32> undef, <2 x i32> <i32 0, i32 1>
    %4 = sitofp <2 x i32> %3 to <2 x float>
    %5 = fdiv <2 x float> <float -0.5, float -0.5>, %4

    %6 = insertelement <2 x float> undef, float %x, i32 0
    %7 = insertelement <2 x float> %6, float %y, i32 1
    %8 = fadd <2 x float> %7, %5
    ret <2 x float> %8
}

define <2 x float> @llpc.patch.image.gather.coordinate.3d(
    <8 x i32> %resource, float %x, float %y) #0
{
    ; Get image width and height
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.3d.v4f32.i32(i32 15,
                                                                     i32 0,
                                                                     <8 x i32> %resource,
                                                                     i32 0,
                                                                     i32 0)

    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = shufflevector  <4 x i32> %2, <4 x i32> undef, <2 x i32> <i32 0, i32 1>
    %4 = sitofp <2 x i32> %3 to <2 x float>
    %5 = fdiv <2 x float> <float -0.5, float -0.5>, %4

    %6 = insertelement <2 x float> undef, float %x, i32 0
    %7 = insertelement <2 x float> %6, float %y, i32 1
    %8 = fadd <2 x float> %7, %5
    ret <2 x float> %8
}

define <2 x float> @llpc.patch.image.gather.coordinate.2darray(
    <8 x i32> %resource, float %x, float %y) #0
{
    ; Get image width and height
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.2darray.v4f32.i32(i32 15,
                                                                          i32 0,
                                                                          <8 x i32> %resource,
                                                                          i32 0,
                                                                          i32 0)

    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = shufflevector  <4 x i32> %2, <4 x i32> undef, <2 x i32> <i32 0, i32 1>
    %4 = sitofp <2 x i32> %3 to <2 x float>
    %5 = fdiv <2 x float> <float -0.5, float -0.5>, %4

    %6 = insertelement <2 x float> undef, float %x, i32 0
    %7 = insertelement <2 x float> %6, float %y, i32 1
    %8 = fadd <2 x float> %7, %5
    ret <2 x float> %8
}

define <2 x float> @llpc.patch.image.gather.coordinate.skip(
    <8 x i32> %resource, float %x, float %y) #0
{
    %1 = insertelement <2 x float> undef, float %x, i32 0
    %2 = insertelement <2 x float> %1, float %y, i32 1
    ret <2 x float> %2
}

define <8 x i32> @llpc.patch.image.gather.descriptor.u32(
    <8 x i32> %resource) #0
{
    %1 = extractelement <8 x i32> %resource, i32 1

    ; Change NUM_FORMAT from UINT to USCALE 134217728 = 0x08000000
    %2 = sub i32 %1, 134217728
    %3 = insertelement <8 x i32> %resource, i32 %2, i32 1
    ret <8 x i32> %3
}

define <8 x i32> @llpc.patch.image.gather.descriptor.i32(
   <8 x i32> %resource) #0
{
    %1 = extractelement <8 x i32> %resource, i32 1

    ; Change NUM_FORMAT from SINT to SSCALE 134217728 = 0x08000000
    %2 = sub i32 %1, 134217728
    %3 = insertelement <8 x i32> %resource, i32 %2, i32 1
    ret <8 x i32> %3
}

define <4 x float> @llpc.patch.image.gather.texel.u32(
    <8 x i32> %resource, <4 x float> %result) #0
{
    %1 = extractelement <8 x i32> %resource, i32 1
    %2 = call i1 @llpc.patch.image.gather.check(<8 x i32> %resource)

    %3 = fptoui <4 x float> %result to <4 x i32>
    %4 = bitcast <4 x i32> %3 to <4 x float>
    %5 = select i1 %2, <4 x float> %4, <4 x float> %result
    ret <4 x float> %5
}

define <4 x float> @llpc.patch.image.gather.texel.i32(
    <8 x i32> %resource, <4 x float> %result) #0
{
    %1 = extractelement <8 x i32> %resource, i32 1
    %2 = call i1 @llpc.patch.image.gather.check(<8 x i32> %resource)

    %3 = fptosi <4 x float> %result to <4 x i32>
    %4 = bitcast <4 x i32> %3 to <4 x float>
    %5 = select i1 %2, <4 x float> %4, <4 x float> %result
    ret <4 x float> %5
}

; Dimension aware version of fetching fmask value
define i32 @llpc.image.fetch.u32.2D.fmaskvalue.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, <2 x i32> %coord, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %fmask = call <8 x i32> @llpc.descriptor.load.fmask(i32 %resourceDescSet,
                                                        i32 %resourceBinding,
                                                        i32 %resourceIdx,
                                                        i32 %imageCallMeta)
    %1 = extractelement <2 x i32> %coord, i32 0
    %2 = extractelement <2 x i32> %coord, i32 1
    %3 = call <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i32(i32 15,
                                                               i32 %1,
                                                               i32 %2,
                                                               <8 x i32> %fmask,
                                                               i32 0,
                                                               i32 0)
    %4 = bitcast <4 x float> %3 to <4 x i32>
    %5 = extractelement <4 x i32> %4, i32 0
    ret i32 %5
}

define i32 @llpc.image.fetch.u32.2DArray.fmaskvalue.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, <3 x i32> %coord, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %fmask = call <8 x i32> @llpc.descriptor.load.fmask(i32 %resourceDescSet,
                                                        i32 %resourceBinding,
                                                        i32 %resourceIdx,
                                                        i32 %imageCallMeta)
    %1 = extractelement <3 x i32> %coord, i32 0
    %2 = extractelement <3 x i32> %coord, i32 1
    %3 = extractelement <3 x i32> %coord, i32 2
    ; NOTE: When loading FMask, DA flag should not be set, 2darray intrinsic will set DA flag, so here use 3d intrinsic instead
    %4 = call <4 x float> @llvm.amdgcn.image.load.3d.v4f32.i32(i32 15,
                                                               i32 %1,
                                                               i32 %2,
                                                               i32 %3,
                                                               <8 x i32> %fmask,
                                                               i32 0,
                                                               i32 0)
    %5 = bitcast <4 x float> %4 to <4 x i32>
    %6 = extractelement <4 x i32> %5, i32 0
    ret i32 %6
}

define i32 @llpc.image.fetch.u32.SubpassData.fmaskvalue.dimaware(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, <2 x i32> %coord, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %fmask = call <8 x i32> @llpc.descriptor.load.fmask(i32 %resourceDescSet,
                                                        i32 %resourceBinding,
                                                        i32 %resourceIdx,
                                                        i32 %imageCallMeta)
    %1 = extractelement <2 x i32> %coord, i32 0
    %2 = extractelement <2 x i32> %coord, i32 1
    %3 = call <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i32(i32 15,
                                                               i32 %1,
                                                               i32 %2,
                                                               <8 x i32> %fmask,
                                                               i32 0,
                                                               i32 0)
    %4 = bitcast <4 x float> %3 to <4 x i32>
    %5 = extractelement <4 x i32> %4, i32 0
    ret i32 %5
}

define <4 x float> @llpc.image.transformCubeGrad(
    float %cubeId,
    float %cubeMa,
    float %faceCoordX,
    float %faceCoordY,
    float %gradX.x,
    float %gradX.y,
    float %gradX.z,
    float %gradY.x,
    float %gradY.y,
    float %gradY.z
    ) #0
{
    ; When sampling cubemap with explicit gradient value, API supplied gradients are cube vectors,
    ; need to transform them to face gradients for the selected face.
    ; Mapping of MajorAxis, U-Axis, V-Axis is (according to DXSDK doc and refrast):
    ;   face_id | MajorAxis | FaceUAxis | FaceVAxis
    ;   0       | +X        | -Z        | -Y
    ;   1       | -X        | +Z        | -Y
    ;   2       | +Y        | +X        | +Z
    ;   3       | -Y        | +X        | -Z
    ;   4       | +Z        | +X        | -Y
    ;   5       | -Z        | -X        | -Y
    ;   (Major Axis is defined by enum D3D11_TEXTURECUBE_FACE in d3d ddk header file (d3d11.h in DX11DDK).)
    ;
    ; Parameters used to convert cube gradient vector to face gradient (face ids are in floats because HW returns
    ; floats):
    ;   face_id | faceidPos    | faceNeg   | flipU | flipV
    ;   0.0     | 0.0          | false     | true  | true
    ;   1.0     | 0.0          | true      | false | true
    ;   2.0     | 1.0          | false     | false | false
    ;   3.0     | 1.0          | true      | false | true
    ;   4.0     | 2.0          | false     | false | true
    ;   5.0     | 2.0          | true      | true  | true

    ; faceidHalf = faceid * 0.5
    %1 = fmul float %cubeId, 0.5
    ; faceidPos = round_zero(faceidHalf)
    ;   faceidPos is: 0.0 (X axis) when face id is 0.0 or 1.0;
    ;                 1.0 (Y axis) when face id is 2.0 or 3.0;
    ;                 2.0 (Z axis) when face id is 4.0 or 5.0;
    %2 = call float @llvm.trunc.f32(float %1)
    ; faceNeg = (faceIdPos != faceIdHalf)
    ;   faceNeg is true when major axis is negative, this corresponds to             face id being 1.0, 3.0, or 5.0
    %3 = fcmp one float %2, %1
    ; faceIsY = (faceidPos == 1.0);
    %4 = fcmp oeq float %2, 1.0
    ; flipU is true when U-axis is negative, this corresponds to face id being 0.0 or 5.0.
    %5 = fcmp oeq float %cubeId, 5.0
    %6 = fcmp oeq float %cubeId, 0.0
    %7 = or i1 %5, %6
    ; flipV is true when V-axis is negative, this corresponds to face id being             anything other than 2.0.
    ; flipV = (faceid != 2.0);
    %8 = fcmp one float %cubeId, 2.0
    ; major2.x = 1/major.x * 1/major.x * 0.5;
    ;          = 1/(2*major.x) * 1/(2*major.x) * 2
    %9 = fdiv float 1.0, %cubeMa
    %10 = fmul float %9, %9
    %11 = fmul float %10, 2.0
    ; majorDeriv.x = (faceidPos == 0.0) ? grad.x : grad.z;
    %12 = fcmp oeq float %2, 0.0
    %13 = select i1 %12, float %gradX.x, float %gradX.z
    ; majorDeriv.x = (faceIsY == 0) ? majorDeriv.x : grad.y;
    %14 = icmp eq i1 %4, 0
    %15 = select i1 %14, float %13, float %gradX.y
    ; majorDeriv.x = (faceNeg == 0.0) ? majorDeriv.x : (-majorDeriv.x);
    %16 = icmp eq i1 %3, 0
    %17 = fmul float %15, -1.0
    %18 = select i1 %16, float %15, float %17
    ; faceDeriv.x = (faceidPos == 0.0) ? grad.z : grad.x;
    %19 = fcmp oeq float %2, 0.0
    %20 = select i1 %19, float %gradX.z, float %gradX.x
    ; faceDeriv.x = (flipU == 0) ? faceDeriv.x : (-faceDeriv.x);
    %21 = icmp eq i1 %7, 0
    %22 = fmul float %20, -1.0
    %23 = select i1 %21, float %20, float %22
    ; faceDeriv.y = (faceIsY == 0) ? grad.y : grad.z;
    %24 = icmp eq i1 %4, 0
    %25 = select i1 %24, float %gradX.y, float %gradX.z
    ; faceDeriv.y = (flipV == 0) ? faceDeriv.y : (-faceDeriv.y);
    %26 = icmp eq i1 %8, 0
    %27 = fmul float %25, -1.0
    %28 = select i1 %26, float %25, float %27
    ; faceDeriv.xy = major.xx * faceDeriv.xy;
    %29 = fmul float %cubeMa, 0.5
    %30 = fmul float %23, %29
    %31 = fmul float %28, %29
    ; faceDeriv.xy = (-faceCrd.xy) * majorDeriv.xx + faceDeriv.xy;
    %32 = fmul float %faceCoordX, -1.0
    %33 = fmul float %faceCoordY, -1.0
    %34 = fmul float %32, %18
    %35 = fmul float %33, %18
    %36 = fadd float %34, %30
    %37 = fadd float %35, %31
    ; grad.xy = faceDeriv.xy * major2.xx;
    %38 = fmul float %36, %11
    %39 = fmul float %37, %11
    ; majorDeriv.x = (faceidPos == 0.0) ? grad.x : grad.z;
    %40 = fcmp oeq float %2, 0.0
    %41 = select i1 %40, float %gradY.x, float %gradY.z
    ; majorDeriv.x = (faceIsY == 0) ? majorDeriv.x : grad.y;
    %42 = icmp eq i1 %4, 0
    %43 = select i1 %42, float %41, float %gradY.y
    ; majorDeriv.x = (faceNeg == 0.0) ? majorDeriv.x : (-majorDeriv.x);
    %44 = icmp eq i1 %3, 0
    %45 = fmul float %43, -1.0
    %46 = select i1 %44, float %43, float %45
    ; faceDeriv.x = (faceidPos == 0.0) ? grad.z : grad.x;
    %47 = fcmp oeq float %2, 0.0
    %48 = select i1 %47, float %gradY.z, float %gradY.x
    ; faceDeriv.x = (flipU == 0) ? faceDeriv.x : (-faceDeriv.x);
    %49 = icmp eq i1 %7, 0
    %50 = fmul float %48, -1.0
    %51 = select i1 %49, float %48, float %50
    ; faceDeriv.y = (faceIsY == 0) ? grad.y : grad.z;
    %52 = icmp eq i1 %4, 0
    %53 = select i1 %52, float %gradY.y, float %gradY.z
    ; faceDeriv.y = (flipV == 0) ? faceDeriv.y : (-faceDeriv.y);
    %54 = icmp eq i1 %8, 0
    %55 = fmul float %53, -1.0
    %56 = select i1 %54, float %53, float %55
    ; faceDeriv.xy = major.xx * faceDeriv.xy;
    %57 = fmul float %cubeMa, 0.5
    %58 = fmul float %51, %57
    %59 = fmul float %56, %57
    ; faceDeriv.xy = (-faceCrd.xy) * majorDeriv.xx + faceDeriv.xy;
    %60 = fmul float %faceCoordX, -1.0
    %61 = fmul float %faceCoordY, -1.0
    %62 = fmul float %60, %46
    %63 = fmul float %61, %46
    %64 = fadd float %62, %58
    %65 = fadd float %63, %59
    ; grad.xy = faceDeriv.xy * major2.xx;
    %66 = fmul float %64, %11
    %67 = fmul float %65, %11
    %68 = insertelement <4 x float> undef, float %38, i32 0
    %69 = insertelement <4 x float> %68, float %39, i32 1
    %70 = insertelement <4 x float> %69, float %66, i32 2
    %71 = insertelement <4 x float> %70, float %67, i32 3
    ret <4 x float> %71
}

define i1 @llpc.imagesparse.texel.resident(
    i32 %residentCode
    ) #1
{
    %1 = icmp eq i32 %residentCode, 0
    ret i1 %1
}

declare <8 x i32> @llpc.descriptor.load.fmask(i32 , i32 , i32, i32) #0

declare <8 x i32> @llpc.descriptor.load.resource(i32 , i32 , i32, i32) #0

declare <4 x i32> @llpc.descriptor.load.texelbuffer(i32 , i32 , i32, i32) #0

declare <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 , <8 x i32> , i32, i1, i1, i1, i1) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.1d.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.2d.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.3d.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.cube.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.1darray.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.2darray.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.2dmsaa.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

declare <4 x float> @llvm.amdgcn.image.getresinfo.2darraymsaa.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #1

declare <4 x float> @llvm.amdgcn.image.load.v4f32.v4i32.v8i32(<4 x i32>, <8 x i32>,  i32, i1, i1, i1, i1) #0

declare i32 @llvm.amdgcn.ubfe.i32(i32, i32, i32) #1

declare float @llvm.trunc.f32(float) #2

declare <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i32(i32, i32, i32, <8 x i32>, i32, i32) #0
declare <4 x float> @llvm.amdgcn.image.load.3d.v4f32.i32(i32, i32, i32, i32, <8 x i32>, i32, i32) #0

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readonly }
