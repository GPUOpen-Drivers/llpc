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

; NOTE: On some hardware, when %y is literal value and less than 0xFFFF, i32 mod will be optimized to i16 mod, there is
; an existing issue in backend which make i16 mod not working, this is the workaround to this issue.
; GLSL: int = int % int
define i32 @llpc.mod.i32(i32 %x, i32 %y) #0
{
    ; Get a non-literal 0 value
    %1 = call i64 @llvm.amdgcn.s.getpc()
    %2 = bitcast i64 %1 to <2 x i32>
    %3 = extractelement <2 x i32> %2, i32 1
    %4 = lshr i32 %3, 15
    ; Add the non-literal 0 value to %y, to turn off the int16 mod optimization
    %5 = add i32 %y, %4

    %6 = srem i32 %x, %5
    %7 = add i32 %6, %5
    ; Check if the signedness of x and y are the same.
    %8 = xor i32 %x, %5
    ; if negative, slt signed less than
    %9 = icmp slt i32 %8, 0
    ; Check if the remainder is not 0.
    %10 = icmp ne i32 %6, 0
    %11 = and i1 %9, %10
    %12 = select i1 %11, i32 %7, i32 %6
    ret i32 %12
}

declare i64 @llvm.amdgcn.s.getpc() #0

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
