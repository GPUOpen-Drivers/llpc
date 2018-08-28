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

; =====================================================================================================================
; >>> Jump Statement
; =====================================================================================================================

; GLSL: void kill()
define spir_func void @_Z4Killv() #0
{
    call void @llvm.AMDGPU.kill(float -1.0)
    ret void
}

; =====================================================================================================================
; >>>  Derivative Functions
; =====================================================================================================================

; GLSL: float dFdx(float)
define float @llpc.dpdx.f32(float %p) #0
{
    ; Broadcast channel 1 to whole quad (32853 = 0x8055)
    %p.i32 = bitcast float %p to i32
    %p0.i32 = call i32 @llvm.amdgcn.ds.swizzle(i32 %p.i32, i32 32853)
    %p0.i32.wqm = call i32 @llvm.amdgcn.wqm.i32(i32 %p0.i32)
    %p0 = bitcast i32 %p0.i32.wqm to float
    ; Broadcast channel 0 to whole quad (32768 = 0x8000)
    %p1.i32 = call i32 @llvm.amdgcn.ds.swizzle(i32 %p.i32, i32 32768)
    %p1.i32.wqm = call i32 @llvm.amdgcn.wqm.i32(i32 %p1.i32)
    %p1 = bitcast i32 %p1.i32.wqm to float

    ; Calculate the delta value
    %dpdx = fsub float %p0, %p1

    ret float %dpdx
}

; GLSL: float dFdy(float)
define float @llpc.dpdy.f32(float %p) #0
{
    ; Broadcast channel 2 to whole quad (32938 = 0x80AA)
    %p.i32 = bitcast float %p to i32
    %p0.i32 = call i32 @llvm.amdgcn.ds.swizzle(i32 %p.i32, i32 32938)
    %p0.i32.wqm = call i32 @llvm.amdgcn.wqm.i32(i32 %p0.i32)
    %p0 = bitcast i32 %p0.i32.wqm to float
    ; Broadcast channel 0 to whole quad (32768 = 0x8000)
    %p1.i32 = call i32 @llvm.amdgcn.ds.swizzle(i32 %p.i32, i32 32768)
    %p1.i32.wqm = call i32 @llvm.amdgcn.wqm.i32(i32 %p1.i32)
    %p1 = bitcast i32 %p1.i32.wqm to float

    ; Calculate the delta value
    %dpdy = fsub float %p0, %p1

    ret float %dpdy
}

; GLSL: float fwidth(float)
define float @llpc.fwidth.f32(float %p) #0
{
    %1 = call float @llpc.dpdx.f32(float %p)
    %2 = call float @llpc.dpdy.f32(float %p)
    %3 = call float @llvm.fabs.f32(float %1)
    %4 = call float @llvm.fabs.f32(float %2)
    %5 = fadd float %3, %4
    ret float %5
}

; GLSL: float dFdxFine(float)
define float @llpc.dpdxFine.f32(float %p) #0
{
    ; Swizzle channels in quad (1 -> 0, 1 -> 1, 3 -> 2, 3 -> 3) (33013 = 0x80F5)
    %p.i32 = bitcast float %p to i32
    %p0.i32 = call i32 @llvm.amdgcn.ds.swizzle(i32 %p.i32, i32 33013)
    %p0.i32.wqm = call i32 @llvm.amdgcn.wqm.i32(i32 %p0.i32)
    %p0 = bitcast i32 %p0.i32.wqm to float
    ; Swizzle channels in quad (0 -> 0, 0 -> 1, 2 -> 2, 2 -> 3) (32928 = 0x80A0)
    %p1.i32 = call i32 @llvm.amdgcn.ds.swizzle(i32 %p.i32, i32 32928)
    %p1.i32.wqm = call i32 @llvm.amdgcn.wqm.i32(i32 %p1.i32)
    %p1 = bitcast i32 %p1.i32.wqm to float

    ; Calculate the delta value
    %dpdx = fsub float %p0, %p1

    ret float %dpdx
}

; GLSL: float dFdyFine(float)
define float @llpc.dpdyFine.f32(float %p) #0
{
    ; Swizzle channels in quad (2 -> 0, 3 -> 1, 2 -> 2, 3 -> 3) (33006 = 0x80EE)
    %p.i32 = bitcast float %p to i32
    %p0.i32 = call i32 @llvm.amdgcn.ds.swizzle(i32 %p.i32, i32 33006)
    %p0.i32.wqm = call i32 @llvm.amdgcn.wqm.i32(i32 %p0.i32)
    %p0 = bitcast i32 %p0.i32.wqm to float
    ; Swizzle channels in quad (0 -> 0, 1 -> 1, 0 -> 2, 1 -> 3) (32836 = 0x8044)
    %p1.i32 = call i32 @llvm.amdgcn.ds.swizzle(i32 %p.i32, i32 32836)
    %p1.i32.wqm = call i32 @llvm.amdgcn.wqm.i32(i32 %p1.i32)
    %p1 = bitcast i32 %p1.i32.wqm to float

    ; Calculate the delta value
    %dpdy = fsub float %p0, %p1

    ret float %dpdy
}

; GLSL: float fwidthFine(float)
define float @llpc.fwidthFine.f32(float %p) #0
{
    %1 = call float @llpc.dpdxFine.f32(float %p)
    %2 = call float @llpc.dpdyFine.f32(float %p)
    %3 = call float @llvm.fabs.f32(float %1)
    %4 = call float @llvm.fabs.f32(float %2)
    %5 = fadd float %3, %4
    ret float %5
}

; GLSL: float dFdxCoarse(float)
define float @llpc.dpdxCoarse.f32(float %p) #0
{
    %1 = call float @llpc.dpdx.f32(float %p)
    ret float %1
}

; GLSL: float dFdyCoarse(float)
define float @llpc.dpdyCoarse.f32(float %p) #0
{
    %1 = call float @llpc.dpdy.f32(float %p)
    ret float %1
}

; GLSL: float fwidthCoarse(float)
define float @llpc.fwidthCoarse.f32(float %p) #0
{
    %1 = call float @llpc.dpdxCoarse.f32(float %p)
    %2 = call float @llpc.dpdyCoarse.f32(float %p)
    %3 = call float @llvm.fabs.f32(float %1)
    %4 = call float @llvm.fabs.f32(float %2)
    %5 = fadd float %3, %4
    ret float %5
}

; =====================================================================================================================
; >>>  Geometry Shader Functions
; =====================================================================================================================

; GLSL: void EmitStreamVertex(int)
define spir_func void @_Z16EmitStreamVertexi(i32 %stream) #0
{
    ; TODO: Multiple output streams are not supported.
    %cmp = icmp eq i32 %stream, 0
    br i1 %cmp, label %.sendmsg, label %.end

.sendmsg:
    ; [9:8] = stream
    %1 = shl i32 %stream, 8
    ; 34 = 0x22, [3:0] = 2 (GS), [5:4] = 2 (emit)
    %2 = or i32 %1, 34
    ; BuiltInWaveId (268435466 = 0x1000000A)
    %3 = call i32 @llpc.input.import.builtin.GsWaveId(i32 268435466)
    call void @llvm.amdgcn.s.sendmsg(i32 %2, i32 %3)
    br label %.end

.end:
    ret void
}

; GLSL: void EndStreamPrimitive(int)
define spir_func void @_Z18EndStreamPrimitivei(i32 %stream) #0
{
    ; TODO: Multiple output streams are not supported.
    %cmp = icmp eq i32 %stream, 0
    br i1 %cmp, label %.sendmsg, label %.end

.sendmsg:
    ; [9:8] = stream
    %1 = shl i32 %stream, 8
    ; 18 = 0x12, [3:0] = 2 (GS), [5:4] = 1 (cut)
    %2 = or i32 %1, 18
    ; BuiltInWaveId (268435466 = 0x1000000A)
    %3 = call i32 @llpc.input.import.builtin.GsWaveId(i32 268435466)
    call void @llvm.amdgcn.s.sendmsg(i32 %2, i32 %3)
    br label %.end

.end:
    ret void
}

; GLSL: void EmitVertex()
define spir_func void @_Z10EmitVertexv() #0
{
    ; BuiltInWaveId (268435466 = 0x1000000A)
    %1 = call i32 @llpc.input.import.builtin.GsWaveId(i32 268435466)
    ; 34 = 0x22, [3:0] = 2 (GS), [5:4] = 2 (emit), [9:8] = 0 (stream = 0)
    call void @llvm.amdgcn.s.sendmsg(i32 34, i32 %1)
    ret void
}

; GLSL: void EndPrimitive()
define spir_func void @_Z12EndPrimitivev() #0
{
    ; BuiltInWaveId (268435466 = 0x1000000A)
    %1 = call i32 @llpc.input.import.builtin.GsWaveId(i32 268435466)
    ; 18 = 0x12, [3:0] = 2 (GS), [5:4] = 1 (cut), [9:8] = 0 (stream = 0)
    call void @llvm.amdgcn.s.sendmsg(i32 18, i32 %1)
    ret void
}

; =====================================================================================================================
; >>>  Shader Invocation Control Functions
; =====================================================================================================================

; GLSL: void barrier() (non compute shader)
define spir_func void @_Z17sub_group_barrierji(i32 %semantics, i32 %scope) #0
{
    call void @llvm.amdgcn.s.barrier()
    ret void
}

; GLSL: void barrier() (compute shader)
define spir_func void @_Z18work_group_barrierji(i32 %semantics, i32 %scope) #0
{
    call void @llvm.amdgcn.s.barrier()
    ret void
}

; GLSL: void barrier() (compute shader)
define spir_func void @_Z7barrierj(i32 %semantics) #0
{
    call void @llvm.amdgcn.s.barrier()
    ret void
}

; =====================================================================================================================
; >>>  Shader Memory Control Functions
; =====================================================================================================================

; GLSL: void memoryBarrier()
;       void memoryBarrierBuffer()
;       void memoryBarrierShared()
;       void memoryBarrierImage()
;       void groupMemoryBarrier()
define spir_func void @_Z9mem_fencej(i32 %semantics) #0
{
    ; TODO: We should choose correct waitcnt() and barrier() according to the specified memory semantics,
    ; Currently, the semantics is ignored and we sync all.
    ; 3952 = 0xF70, [3:0] = vm_cnt, [6:4] = exp_cnt, [11:8] = lgkm_cnt
    call void @llvm.amdgcn.s.waitcnt(i32 3952)
    call void @llvm.amdgcn.s.barrier()
    ret void
}

; =====================================================================================================================
; >>>  Interpolation Functions
; =====================================================================================================================

; Adjust interpolation I/J according to specified offsets X/Y
define float @llpc.input.interpolate.adjustij.f32(float %ij, float %offsetX, float %offsetY)
{
    ; Calculate DpDx, DpDy for %ij
    %1 = call float @llpc.dpdxFine.f32(float %ij)
    %2 = call float @llpc.dpdyFine.f32(float %ij)

    ; Adjust %ij by offset
    %3 = fmul float %offsetX, %1
    %4 = fadd float %ij, %3
    %5 = fmul float %offsetY, %2
    %6 = fadd float %4, %5

    ret float %6
}

; Evaluate interpolation I/J for GLSL function interpolateAtOffset()
define <2 x float> @llpc.input.interpolate.evalij.offset.v2f32(<2 x float> %offset) #0
{
    ; BuiltInInterpPullMode 268435459 = 0x10000003
    %1 = call <3 x float> @llpc.input.import.builtin.InterpPullMode(i32 268435459)
    ; Extract Pull Model I/W, J/W, 1/W
    %2 = extractelement <3 x float> %1, i32 0
    %3 = extractelement <3 x float> %1, i32 1
    %4 = extractelement <3 x float> %1, i32 2

    ; Extract offset to scalar
    %5 = extractelement <2 x float> %offset, i32 0
    %6 = extractelement <2 x float> %offset, i32 1

    ; Adjust each coefficient by offset
    %7 = call float @llpc.input.interpolate.adjustij.f32(float %2, float %5, float %6)
    %8 = call float @llpc.input.interpolate.adjustij.f32(float %3, float %5, float %6)
    %9 = call float @llpc.input.interpolate.adjustij.f32(float %4, float %5, float %6)

    ; Get final I, J
    %10 = fmul float %7, %9
    %11 = fmul float %8, %9

    %12 = insertelement <2 x float> undef, float %10, i32 0
    %13 = insertelement <2 x float> %12, float %11, i32 1

    ret <2 x float> %13
}

; Evaluate interpolation I/J for GLSL function interpolateAtOffset() with "noperspective" qualifier specified
; on interpolant
define <2 x float> @llpc.input.interpolate.evalij.offset.noperspective.v2f32(<2 x float> %offset) #0
{
    ; BuiltInInterpLinearCenter 268435461 = 0x10000005
    %1 = call <2 x float> @llpc.input.import.builtin.InterpLinearCenter(i32 268435461)
    ; Extract I, J
    %2 = extractelement <2 x float> %1, i32 0
    %3 = extractelement <2 x float> %1, i32 1

    ; Extract offset to scalar
    %4 = extractelement <2 x float> %offset, i32 0
    %5 = extractelement <2 x float> %offset, i32 1

    ; Adjust I,J by offset
    %6 = call float @llpc.input.interpolate.adjustij.f32(float %2, float %4, float %5)
    %7 = call float @llpc.input.interpolate.adjustij.f32(float %3, float %4, float %5)

    %8 = insertelement <2 x float> undef, float %6, i32 0
    %9 = insertelement <2 x float> %8, float %7, i32 1

    ret <2 x float> %9
}

; Evaluate interpolation I/J for GLSL function interpolateAtSample()
define <2 x float> @llpc.input.interpolate.evalij.sample(i32 %sample) #0
{
    ; BuiltInSamplePosOffset 268435463 = 0x10000007
    %1 = call <2 x float> @llpc.input.import.builtin.SamplePosOffset(i32 268435463, i32 %sample)
    %2 = call <2 x float> @llpc.input.interpolate.evalij.offset.v2f32(<2 x float> %1)
    ret <2 x float> %2
}

; Evaluate interpolation I/J for GLSL function interpolateAtSample() with "noperspective" qualifier specified
; on interpolant
define <2 x float> @llpc.input.interpolate.evalij.sample.noperspective(i32 %sample) #0
{
    ; BuiltInSamplePosOffset 268435463 = 0x10000007
    %1 = call <2 x float> @llpc.input.import.builtin.SamplePosOffset(i32 268435463, i32 %sample)
    %2 = call <2 x float> @llpc.input.interpolate.evalij.offset.noperspective.v2f32(<2 x float> %1)
    ret <2 x float> %2
}

; =====================================================================================================================
; >>>  Functions of Extension AMD_gcn_shader
; =====================================================================================================================

; GLSL: float cubeFaceIndex(vec3)
define spir_func float @_Z16CubeFaceIndexAMDDv3_f(<3 x float> %coord)
{
    %1 = extractelement <3 x float> %coord, i32 0
    %2 = extractelement <3 x float> %coord, i32 1
    %3 = extractelement <3 x float> %coord, i32 2

    %4 = call float @llvm.amdgcn.cubeid(float %1, float %2, float %3)
    ret float %4
}

; GLSL: vec2 cubeFaceCoord(vec3)
define spir_func <2 x float> @_Z16CubeFaceCoordAMDDv3_f(<3 x float> %coord)
{
    %1 = extractelement <3 x float> %coord, i32 0
    %2 = extractelement <3 x float> %coord, i32 1
    %3 = extractelement <3 x float> %coord, i32 2

    %4 = call float @llvm.amdgcn.cubema(float %1, float %2, float %3)
    %5 = fdiv float 1.0, %4

    %6 = call float @llvm.amdgcn.cubesc(float %1, float %2, float %3)
    %7 = fmul float %5, %6
    %8 = fadd float %7, 0.5

    %9  = call float @llvm.amdgcn.cubetc(float %1, float %2, float %3)
    %10 = fmul float %5, %9
    %11 = fadd float %10, 0.5

    %12 = insertelement <2 x float> undef, float %8, i32 0
    %13 = insertelement <2 x float> %12, float %11, i32 1

    ret <2 x float> %13
}

; GLSL: uint64_t time()
define spir_func i64 @_Z7TimeAMDv()
{
    %1 = call i64 @llvm.amdgcn.s.memtime() #0
    ; Prevent optimization of backend compiler on the control flow
    %2 = call i64 asm sideeffect "; %1", "=v,0"(i64 %1)
    ret i64 %2
}

declare void @llvm.AMDGPU.kill(float) #0
declare float @llvm.fabs.f32(float) #0
declare i32 @llvm.amdgcn.ds.swizzle(i32, i32) #2
declare void @llvm.amdgcn.s.waitcnt(i32) #0
declare void @llvm.amdgcn.s.barrier() #3
declare void @llvm.amdgcn.s.sendmsg(i32, i32) #0
declare <3 x float> @llpc.input.import.builtin.InterpPullMode(i32) #0
declare <2 x float> @llpc.input.import.builtin.InterpLinearCenter(i32) #0
declare <2 x float> @llpc.input.import.builtin.SamplePosOffset(i32, i32) #0
declare i32 @llpc.input.import.builtin.GsWaveId(i32) #0
declare i32 @llvm.amdgcn.readlane(i32, i32) #2
declare i32 @llvm.amdgcn.readfirstlane(i32) #2
declare i32 @llvm.amdgcn.writelane(i32, i32, i32) #2
declare i64 @llvm.amdgcn.icmp.i32(i32, i32, i32) #2
declare i32 @llvm.amdgcn.mbcnt.lo(i32, i32) #1
declare i32 @llvm.amdgcn.mbcnt.hi(i32, i32) #1
declare i1 @llvm.amdgcn.ps.live() #1
declare i64 @llvm.cttz.i64(i64, i1) #0
declare i64 @llvm.ctlz.i64(i64, i1) #0
declare i64 @llvm.ctpop.i64(i64) #0
declare float @llvm.amdgcn.cubeid(float, float, float) #1
declare float @llvm.amdgcn.cubesc(float, float, float) #1
declare float @llvm.amdgcn.cubema(float, float, float) #1
declare float @llvm.amdgcn.cubetc(float, float, float) #1
declare i64 @llvm.amdgcn.s.memtime() #0
declare i32 @llvm.amdgcn.wwm.i32(i32) #1
declare i64 @llvm.amdgcn.wwm.i64(i64) #1
declare <2 x i32> @llvm.amdgcn.wwm.v2i32(<2 x i32>) #1
declare i32 @llvm.amdgcn.set.inactive.i32(i32, i32) #2
declare i64 @llvm.amdgcn.set.inactive.i64(i64, i64) #2
declare <2 x i32> @llvm.amdgcn.set.inactive.v2i32(<2 x i32>, <2 x i32>) #2
declare <3 x i32> @llvm.amdgcn.set.inactive.v3i32(<3 x i32>, <3 x i32>) #2
declare <4 x i32> @llvm.amdgcn.set.inactive.v4i32(<4 x i32>, <4 x i32>) #2
declare i32 @llpc.sminnum.i32(i32, i32) #0
declare i32 @llpc.smaxnum.i32(i32, i32) #0
declare i32 @llpc.uminnum.i32(i32, i32) #0
declare i32 @llpc.umaxnum.i32(i32, i32) #0
declare float @llvm.minnum.f32(float, float) #0
declare float @llvm.maxnum.f32(float, float) #0
declare i32 @llvm.amdgcn.wqm.i32(i32) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readnone convergent }
attributes #3 = { convergent nounwind }
attributes #4 = { nounwind readonly }
