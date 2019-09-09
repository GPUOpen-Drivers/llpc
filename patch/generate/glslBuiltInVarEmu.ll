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
