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

define i32 @llpc.image.querynonlod.sizelod.1D.i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    ret i32 %3
}

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

define <2 x i32> @llpc.image.querynonlod.sizelod.2D.v2i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = insertelement <2 x i32> undef, i32 %3, i32 0
    %6 = insertelement <2 x i32> %5, i32 %4, i32 1
    ret <2 x i32> %6
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

define <2 x i32> @llpc.image.querynonlod.sizelod.Cube.v2i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false)
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

define <3 x i32> @llpc.image.querynonlod.sizelod.3D.v3i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = extractelement <4 x i32> %2, i32 2
    %6 = insertelement <3 x i32> undef, i32 %3, i32 0
    %7 = insertelement <3 x i32> %6, i32 %4, i32 1
    %8 = insertelement <3 x i32> %7, i32 %5, i32 2
    ret <3 x i32> %8
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

define <3 x i32> @llpc.image.querynonlod.sizelod.2DArray.v3i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 true)
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

define <3 x i32> @llpc.image.querynonlod.sizelod.CubeArray.v3i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 true)
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

define <2 x i32> @llpc.image.querynonlod.sizelod.2D.sample.v2i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = insertelement <2 x i32> undef, i32 %3, i32 0
    %6 = insertelement <2 x i32> %5, i32 %4, i32 1
    ret <2 x i32> %6
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

define <3 x i32> @llpc.image.querynonlod.sizelod.2DArray.sample.v3i32(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %lod, i32 %imageCallMeta) #0
{
    %resource = call <8 x i32> @llpc.descriptor.load.resource(i32 %resourceDescSet,
                                                              i32 %resourceBinding,
                                                              i32 %resourceIdx,
                                                              i32 %imageCallMeta)
    %1 = call <4 x float> @llvm.amdgcn.image.getresinfo.v4f32.i32.v8i32(i32 %lod,
                                                                        <8 x i32> %resource,
                                                                        i32 15,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 false,
                                                                        i1 true)
    %2 = bitcast <4 x float> %1 to <4 x i32>
    %3 = extractelement <4 x i32> %2, i32 0
    %4 = extractelement <4 x i32> %2, i32 1
    %5 = extractelement <4 x i32> %2, i32 2
    %6 = insertelement <3 x i32> undef, i32 %3, i32 0
    %7 = insertelement <3 x i32> %6, i32 %4, i32 1
    %8 = insertelement <3 x i32> %7, i32 %5, i32 2
    ret <3 x i32> %8
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

define i32 @llpc.image.querynonlod.sizelod.Buffer.i32(
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

define i32 @llpc.image.querynonlod.levels.1D(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
}

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

define i32 @llpc.image.querynonlod.levels.2D(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
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

define i32 @llpc.image.querynonlod.levels.Cube(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
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

define i32 @llpc.image.querynonlod.levels.1DArray(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
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


define i32 @llpc.image.querynonlod.levels.3D(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
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

define i32 @llpc.image.querynonlod.levels.2DArray(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
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


define i32 @llpc.image.querynonlod.levels.CubeArray(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
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

define i32 @llpc.image.querynonlod.levels.2D.sample(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
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

define i32 @llpc.image.querynonlod.levels.2DArray.sample(
    i32 %resourceDescSet, i32 %resourceBinding, i32 %resourceIdx, i32 %imageCallMeta) #0
{
    %1 = call i32 @llpc.image.querynonlod.levels(i32 %resourceDescSet,
                                                 i32 %resourceBinding,
                                                 i32 %resourceIdx,
                                                 i32 %imageCallMeta)
    ret i32 %1
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
    ret <8 x i32> %resource
}

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

declare i32 @llvm.amdgcn.ubfe.i32(i32, i32, i32) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
