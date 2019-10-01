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

; =====================================================================================================================
; >>>  Common Built-in Variables
; =====================================================================================================================

; GLSL: in uint64_t gl_SubGroupEqMask
define i64 @llpc.input.import.builtin.SubgroupEqMaskKHR.i64.i32(i32 %builtInId) #0
{
    %1 = call i32 @llpc.input.import.builtin.SubgroupLocalInvocationId.i32.i32(i32 41)
    %2 = zext i32 %1 to i64

    ; 1 << threadId
    %3 = shl i64 1, %2

    ret i64 %3
}

; GLSL: in uint64_t gl_SubGroupGeMask
define i64 @llpc.input.import.builtin.SubgroupGeMaskKHR.i64.i32(i32 %builtInId) #0
{
    %1 = call i32 @llpc.input.import.builtin.SubgroupLocalInvocationId.i32.i32(i32 41)
    %2 = call i32 @llpc.input.import.builtin.SubgroupSize.i32.i32(i32 36)
    %3 = icmp eq i32 %2, 64
    br i1 %3, label %.wave64, label %.wave32
.wave64:
    %4 = zext i32 %1 to i64
    ; 0xFFFFFFFF'FFFFFFFF << threadId
    %5 = shl i64 -1, %4
    br label %.end
.wave32:
    ; 0xFFFFFFFF << threadId
    %6 = shl i32 -1, %1
    %7 = zext i32 %6 to i64
    br label %.end
.end:
    %8 = phi i64 [ %5, %.wave64 ], [ %7, %.wave32]
    ret i64 %8
}

; GLSL: in uint64_t gl_SubGroupGtMask
define i64 @llpc.input.import.builtin.SubgroupGtMaskKHR.i64.i32(i32 %builtInId) #0
{
    %1 = call i32 @llpc.input.import.builtin.SubgroupLocalInvocationId.i32.i32(i32 41)
    %2 = call i32 @llpc.input.import.builtin.SubgroupSize.i32.i32(i32 36)
    %3 = icmp eq i32 %2, 64
    br i1 %3, label %.wave64, label %.wave32
.wave64:
    %4 = zext i32 %1 to i64
    ; 0xFFFFFFFF'FFFFFFFF << threadId
    %5 = shl i64 -1, %4
    %6 = shl i64 1, %4
    ; (0xFFFFFFFF'FFFFFFFF << threadId) ^ (1 << threadId)
    %7 = xor i64 %5, %6
    br label %.end
.wave32:
    ; (FFFFFFFF << threadId)
    %8 = shl i32 -1, %1
    ; 1 << threadId
    %9 = shl i32 1, %1
    ; (FFFFFFFF << threadId) ^ (1 << threadId)
    %10 = xor i32 %8, %9

    %11 = zext i32 %10 to i64
    br label %.end
.end:
    %12 = phi i64 [ %7, %.wave64 ], [ %11, %.wave32]
    ret i64 %12
}

; GLSL: in uint64_t gl_SubGroupLeMask
define i64 @llpc.input.import.builtin.SubgroupLeMaskKHR.i64.i32(i32 %builtInId) #0
{
    %1 = call i32 @llpc.input.import.builtin.SubgroupLocalInvocationId.i32.i32(i32 41)
    %2 = zext i32 %1 to i64

    ; 0xFFFFFFFF'FFFFFFFF << threadId
    %3 = shl i64 -1, %2

    ; 1 << threadId
    %4 = shl i64 1, %2

    ; (0xFFFFFFFF'FFFFFFFF << threadId) ^ (1 << threadId)
    %5 = xor i64 %3, %4

    ; ~((0xFFFFFFFF'FFFFFFFF << threadId) ^ (1 << threadId))
    %6 = xor i64 %5, -1

    ret i64 %6
}

; GLSL: in uint64_t gl_SubGroupLtMask
define i64 @llpc.input.import.builtin.SubgroupLtMaskKHR.i64.i32(i32 %builtInId) #0
{
    %1 = call i32 @llpc.input.import.builtin.SubgroupLocalInvocationId.i32.i32(i32 41)
    %2 = zext i32 %1 to i64

    ; 0xFFFFFFFF'FFFFFFFF << threadId
    %3 = shl i64 -1, %2

    ; ~(0xFFFFFFFF'FFFFFFFF << threadId)
    %4 = xor i64 %3, -1

    ret i64 %4
}

; GLSL: in uvec4 gl_SubgroupEqMask
define <4 x i32> @llpc.input.import.builtin.SubgroupEqMask.v4i32.i32(i32 %builtInId) #0
{
    %1 = call i64 @llpc.input.import.builtin.SubgroupEqMaskKHR.i64.i32(i32 %builtInId)
    %2 = bitcast i64 %1 to <2 x i32>
    %3 = shufflevector <2 x i32> %2, <2 x i32> <i32 0, i32 0>, <4 x i32> <i32 0, i32 1, i32 2, i32 3>

    ret <4 x i32> %3
}

; GLSL: in uvec4 gl_SubgroupGeMask
define <4 x i32> @llpc.input.import.builtin.SubgroupGeMask.v4i32.i32(i32 %builtInId) #0
{
    %1 = call i64 @llpc.input.import.builtin.SubgroupGeMaskKHR.i64.i32(i32 %builtInId)
    %2 = bitcast i64 %1 to <2 x i32>
    %3 = shufflevector <2 x i32> %2, <2 x i32> <i32 0, i32 0>, <4 x i32> <i32 0, i32 1, i32 2, i32 3>

    ret <4 x i32> %3
}

; GLSL: in uvec4 gl_SubgroupGtMask
define <4 x i32> @llpc.input.import.builtin.SubgroupGtMask.v4i32.i32(i32 %builtInId) #0
{
    %1 = call i64 @llpc.input.import.builtin.SubgroupGtMaskKHR.i64.i32(i32 %builtInId)
    %2 = bitcast i64 %1 to <2 x i32>
    %3 = shufflevector <2 x i32> %2, <2 x i32> <i32 0, i32 0>, <4 x i32> <i32 0, i32 1, i32 2, i32 3>

    ret <4 x i32> %3
}

; GLSL: in uvec4 gl_SubgroupLeMask
define <4 x i32> @llpc.input.import.builtin.SubgroupLeMask.v4i32.i32(i32 %builtInId) #0
{
    %1 = call i64 @llpc.input.import.builtin.SubgroupLeMaskKHR.i64.i32(i32 %builtInId)
    %2 = bitcast i64 %1 to <2 x i32>
    %3 = shufflevector <2 x i32> %2, <2 x i32> <i32 0, i32 0>, <4 x i32> <i32 0, i32 1, i32 2, i32 3>

    ret <4 x i32> %3
}

; GLSL: in uvec4 gl_SubgroupLtMask
define <4 x i32> @llpc.input.import.builtin.SubgroupLtMask.v4i32.i32(i32 %builtInId) #0
{
    %1 = call i64 @llpc.input.import.builtin.SubgroupLtMaskKHR.i64.i32(i32 %builtInId)
    %2 = bitcast i64 %1 to <2 x i32>
    %3 = shufflevector <2 x i32> %2, <2 x i32> <i32 0, i32 0>, <4 x i32> <i32 0, i32 1, i32 2, i32 3>

    ret <4 x i32> %3
}

; =====================================================================================================================
; >>>  Compute Shader Built-in Variables
; =====================================================================================================================

; GLSL: in uvec3 gl_GloablInvocationID
define <3 x i32> @llpc.input.import.builtin.GlobalInvocationId.v3i32.i32(i32 %builtInId) #0
{
    %1 = call <3 x i32> @llpc.input.import.builtin.WorkgroupSize.v3i32.i32(i32 25)
    %2 = call <3 x i32> @llpc.input.import.builtin.WorkgroupId.v3i32.i32(i32 26)
    %3 = call <3 x i32> @llpc.input.import.builtin.LocalInvocationId.v3i32.i32(i32 27)

    %4 = mul <3 x i32> %1, %2
    %5 = add <3 x i32> %4, %3

    ret <3 x i32> %5
}

; GLSL: in uint gl_LocationInvocationIndex
define i32 @llpc.input.import.builtin.LocalInvocationIndex.i32.i32(i32 %builtInId) #0
{
    %1 = call <3 x i32> @llpc.input.import.builtin.WorkgroupSize.v3i32.i32(i32 25)
    %2 = call <3 x i32> @llpc.input.import.builtin.LocalInvocationId.v3i32.i32(i32 27)

    %3 = extractelement <3 x i32> %1, i32 0 ; gl_WorkGroupSize.x
    %4 = extractelement <3 x i32> %1, i32 1 ; gl_WorkGroupSize.y

    %5 = extractelement <3 x i32> %2, i32 0 ; gl_LocalInvocationID.x
    %6 = extractelement <3 x i32> %2, i32 1 ; gl_LocalInvocationID.y
    %7 = extractelement <3 x i32> %2, i32 2 ; gl_LocalInvocationID.z

    %8 = mul i32 %4, %7
    %9 = add i32 %8, %6
    %10 = mul i32 %3, %9
    %11 = add i32 %10, %5

    ret i32 %11
}

; GLSL: in uint gl_SubgroupID
define i32 @llpc.input.import.builtin.SubgroupId.i32.i32(i32 %builtInId) #0
{
    ; gl_SubgroupID = gl_LocationInvocationIndex / gl_SubgroupSize
    %1 = call i32 @llpc.input.import.builtin.LocalInvocationIndex.i32.i32(i32 29)
    %2 = call i32 @llpc.input.import.builtin.SubgroupSize.i32.i32(i32 36)
    %3 = udiv i32 %1, %2

    ret i32 %3
}

declare i32 @llpc.input.import.builtin.SubgroupLocalInvocationId.i32.i32(i32) #0
declare <3 x i32> @llpc.input.import.builtin.WorkgroupSize.v3i32.i32(i32) #0
declare <3 x i32> @llpc.input.import.builtin.WorkgroupId.v3i32.i32(i32) #0
declare <3 x i32> @llpc.input.import.builtin.LocalInvocationId.v3i32.i32(i32) #0
declare i32 @llpc.input.import.builtin.SubgroupSize.i32.i32(i32) #0
declare i32 @llpc.input.import.builtin.NumSamples.i32.i32(i32) #0
declare i32 @llpc.input.import.builtin.SamplePatternIdx.i32.i32(i32) #0
declare i32 @llpc.input.import.builtin.SampleId.i32.i32(i32) #0
declare <4 x i32> @llpc.descriptor.load.buffer(i32, i32, i32, i1)
declare <8 x i8> @llpc.buffer.load.v8i8(<4 x i32>, i32, i1, i32, i1)

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
