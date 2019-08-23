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

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i8:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024"
target triple = "spir64-unknown-unknown"

; =====================================================================================================================
; >>>  Common Functions
; =====================================================================================================================

; GLSL: int8_t abs(int8_t)
define i8 @llpc.sabs.i8(i8 %x) #0
{
    %nx = sub i8 0, %x
    %con = icmp sgt i8 %x, %nx
    %val = select i1 %con, i8 %x, i8 %nx
    ret i8 %val
}

; GLSL: int8_t sign(int8_t)
define i8 @llpc.ssign.i8(i8 %x) #0
{
    %con1 = icmp sgt i8 %x, 0
    %ret1 = select i1 %con1, i8 1, i8 %x
    %con2 = icmp sge i8 %ret1, 0
    %ret2 = select i1 %con2, i8 %ret1, i8 -1
    ret i8 %ret2
}

; GLSL: int8_t min(int8_t, int8_t)
define i8 @llpc.sminnum.i8(i8 %x, i8 %y) #0
{
    %1 = icmp slt i8 %y, %x
    %2 = select i1 %1, i8 %y, i8 %x
    ret i8 %2
}

; GLSL: int8_t min(int8_t, int8_t)
define spir_func i8 @_Z4smincc(
    i8 %x, i8 %y) #0
{
    %cmp = icmp sle i8 %x, %y
    %val = select i1 %cmp, i8 %x, i8 %y
    ret i8 %val
}

; GLSL: i8vec2 min(i8vec2, i8vec2)
define spir_func <2 x i8> @_Z4sminDv2_cDv2_c(
    <2 x i8> %x, <2 x i8> %y) #0
{
    %cmp = icmp sle <2 x i8> %x, %y
    %val = select <2 x i1> %cmp, <2 x i8> %x, <2 x i8> %y

    ret <2 x i8> %val
}

; GLSL: i8vec3 min(i8vec3, i8vec3)
define spir_func <3 x i8> @_Z4sminDv3_cDv3_c(
    <3 x i8> %x, <3 x i8> %y) #0
{
    %cmp = icmp sle <3 x i8> %x, %y
    %val = select <3 x i1> %cmp, <3 x i8> %x, <3 x i8> %y

    ret <3 x i8> %val
}

; GLSL: i8vec4 min(i8vec4, i8vec4)
define spir_func <4 x i8> @_Z4sminDv4_cDv4_c(
    <4 x i8> %x, <4 x i8> %y) #0
{
    %cmp = icmp sle <4 x i8> %x, %y
    %val = select <4 x i1> %cmp, <4 x i8> %x, <4 x i8> %y

    ret <4 x i8> %val
}

; GLSL: uint8_t min(uint8_t, uint8_t)
define spir_func i8 @_Z4umincc(
    i8 %x, i8 %y) #0
{
    %cmp = icmp ule i8 %x, %y
    %val = select i1 %cmp, i8 %x, i8 %y

    ret i8 %val
}

; GLSL: u8vec2 min(u8vec2, u8vec2)
define spir_func <2 x i8> @_Z4uminDv2_cDv2_c(
    <2 x i8> %x, <2 x i8> %y) #0
{
    %cmp = icmp ule <2 x i8> %x, %y
    %val = select <2 x i1> %cmp, <2 x i8> %x, <2 x i8> %y

    ret <2 x i8> %val
}

; GLSL: u8vec3 min(u8vec3, u8vec3)
define spir_func <3 x i8> @_Z4uminDv3_cDv3_c(
    <3 x i8> %x, <3 x i8> %y) #0
{
    %cmp = icmp ule <3 x i8> %x, %y
    %val = select <3 x i1> %cmp, <3 x i8> %x, <3 x i8> %y

    ret <3 x i8> %val
}

; GLSL: u8vec4 min(u8vec4, u8vec4)
define spir_func <4 x i8> @_Z4uminDv4_cDv4_c(
    <4 x i8> %x, <4 x i8> %y) #0
{
    %cmp = icmp ule <4 x i8> %x, %y
    %val = select <4 x i1> %cmp, <4 x i8> %x, <4 x i8> %y

    ret <4 x i8> %val
}

; GLSL: int8_t max(int8_t, int8_t)
define spir_func i8 @_Z4smaxcc(
    i8 %x, i8 %y) #0
{
    %cmp = icmp sge i8 %x, %y
    %val = select i1 %cmp, i8 %x, i8 %y
    ret i8 %val
}

; GLSL: i8vec2 max(i8vec2, i8vec2)
define spir_func <2 x i8> @_Z4smaxDv2_cDv2_c(
    <2 x i8> %x, <2 x i8> %y) #0
{
    %cmp = icmp sge <2 x i8> %x, %y
    %val = select <2 x i1> %cmp, <2 x i8> %x, <2 x i8> %y

    ret <2 x i8> %val
}

; GLSL: i8vec3 max(i8vec3, i8vec3)
define spir_func <3 x i8> @_Z4smaxDv3_cDv3_c(
    <3 x i8> %x, <3 x i8> %y) #0
{
    %cmp = icmp sge <3 x i8> %x, %y
    %val = select <3 x i1> %cmp, <3 x i8> %x, <3 x i8> %y

    ret <3 x i8> %val
}

; GLSL: i8vec4 max(i8vec4, i8vec4)
define spir_func <4 x i8> @_Z4smaxDv4_cDv4_c(
    <4 x i8> %x, <4 x i8> %y) #0
{
    %cmp = icmp sge <4 x i8> %x, %y
    %val = select <4 x i1> %cmp, <4 x i8> %x, <4 x i8> %y

    ret <4 x i8> %val
}

; GLSL: uint8_t max(uint8_t, uint8_t)
define spir_func i8 @_Z4umaxcc(
    i8 %x, i8 %y) #0
{
    %cmp = icmp uge i8 %x, %y
    %val = select i1 %cmp, i8 %x, i8 %y

    ret i8 %val
}

; GLSL: u8vec2 max(u8vec2, u8vec2)
define spir_func <2 x i8> @_Z4umaxDv2_cDv2_c(
    <2 x i8> %x, <2 x i8> %y) #0
{
    %cmp = icmp uge <2 x i8> %x, %y
    %val = select <2 x i1> %cmp, <2 x i8> %x, <2 x i8> %y

    ret <2 x i8> %val
}

; GLSL: u8vec3 max(u8vec3, u8vec3)
define spir_func <3 x i8> @_Z4umaxDv3_cDv3_c(
    <3 x i8> %x, <3 x i8> %y) #0
{
    %cmp = icmp uge <3 x i8> %x, %y
    %val = select <3 x i1> %cmp, <3 x i8> %x, <3 x i8> %y

    ret <3 x i8> %val
}

; GLSL: u8vec4 max(u8vec4, u8vec4)
define spir_func <4 x i8> @_Z4umaxDv4_cDv4_c(
    <4 x i8> %x, <4 x i8> %y) #0
{
    %cmp = icmp uge <4 x i8> %x, %y
    %val = select <4 x i1> %cmp, <4 x i8> %x, <4 x i8> %y

    ret <4 x i8> %val
}

; GLSL: uint8_t clamp(uint8_t, uint8_t, uint8_t)
define spir_func i8 @_Z6uclampccc(
    i8 %x, i8 %minVal, i8 %maxVal) #0
{
    %1 = call i8 @_Z4umaxcc(i8 %x, i8 %minVal)
    %2 = call i8 @_Z4umincc(i8 %1, i8 %maxVal)
    ret i8 %2
}

; GLSL: u8vec2 clamp(u8vec2, u8vec2, u8vec2)
define spir_func <2 x i8> @_Z6uclampDv2_cDv2_cDv2_c(
    <2 x i8> %x, <2 x i8> %minVal, <2 x i8> %maxVal) #0
{
    %1 = call <2 x i8> @_Z4umaxDv2_cDv2_c(<2 x i8> %x, <2 x i8> %minVal)
    %2 = call <2 x i8> @_Z4uminDv2_cDv2_c(<2 x i8> %1, <2 x i8> %maxVal)
    ret <2 x i8> %2
}

; GLSL: u8vec3 clamp(u8vec3, u8vec3, u8vec3)
define spir_func <3 x i8> @_Z6uclampDv3_cDv3_cDv3_c(
    <3 x i8> %x, <3 x i8> %minVal, <3 x i8> %maxVal) #0
{
    %1 = call <3 x i8> @_Z4umaxDv3_cDv3_c(<3 x i8> %x, <3 x i8> %minVal)
    %2 = call <3 x i8> @_Z4uminDv3_cDv3_c(<3 x i8> %1, <3 x i8> %maxVal)
    ret <3 x i8> %2
}

; GLSL: u8vec4 clamp(u8vec4, u8vec4, u8vec4)
define spir_func <4 x i8> @_Z6uclampDv4_cDv4_cDv4_c(
    <4 x i8> %x, <4 x i8> %minVal, <4 x i8> %maxVal) #0
{
    %1 = call <4 x i8> @_Z4umaxDv4_cDv4_c(<4 x i8> %x, <4 x i8> %minVal)
    %2 = call <4 x i8> @_Z4uminDv4_cDv4_c(<4 x i8> %1, <4 x i8> %maxVal)
    ret <4 x i8> %2
}

; GLSL: int8_t clamp(int8_t, int8_t, int8_t)
define spir_func i8 @_Z6sclampccc(
    i8 %x, i8 %minVal, i8 %maxVal) #0
{
    %1 = call i8 @_Z4smaxcc(i8 %x, i8 %minVal)
    %2 = call i8 @_Z4smincc(i8 %1, i8 %maxVal)
    ret i8 %2
}

; GLSL: i8vec2 clamp(i8vec2, i8vec2, i8vec2)
define spir_func <2 x i8> @_Z6sclampDv2_cDv2_cDv2_c(
    <2 x i8> %x, <2 x i8> %minVal, <2 x i8> %maxVal) #0
{
    %1 = call <2 x i8> @_Z4smaxDv2_cDv2_c(<2 x i8> %x, <2 x i8> %minVal)
    %2 = call <2 x i8> @_Z4sminDv2_cDv2_c(<2 x i8> %1, <2 x i8> %maxVal)
    ret <2 x i8> %2
}

; GLSL: i8vec3 clamp(i8vec3, i8vec3, i8vec3)
define spir_func <3 x i8> @_Z6sclampDv3_cDv3_cDv3_c(
    <3 x i8> %x, <3 x i8> %minVal, <3 x i8> %maxVal) #0
{
    %1 = call <3 x i8> @_Z4smaxDv3_cDv3_c(<3 x i8> %x, <3 x i8> %minVal)
    %2 = call <3 x i8> @_Z4sminDv3_cDv3_c(<3 x i8> %1, <3 x i8> %maxVal)
    ret <3 x i8> %2
}

; GLSL: i8vec4 clamp(i8vec4, i8vec4, i8vec4)
define spir_func <4 x i8> @_Z6sclampDv4_cDv4_cDv4_c(
    <4 x i8> %x, <4 x i8> %minVal, <4 x i8> %maxVal) #0
{
    %1 = call <4 x i8> @_Z4smaxDv4_cDv4_c(<4 x i8> %x, <4 x i8> %minVal)
    %2 = call <4 x i8> @_Z4sminDv4_cDv4_c(<4 x i8> %1, <4 x i8> %maxVal)
    ret <4 x i8> %2
}

attributes #0 = { nounwind }