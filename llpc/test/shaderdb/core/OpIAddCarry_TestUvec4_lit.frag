#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/


layout(location = 0) in vec4 colorIn1;
layout(location = 0) out vec4 color;
void main()
{
    uvec4 bd = uvec4(colorIn1);
    uint outi = 0;
    uvec4 outv = uvec4(0);
    uint out1 = uaddCarry(bd.x,bd.y,outi);
    uvec4 out0 = uaddCarry(bd,bd,outv);
    color = vec4(out0 + outv);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32
; SHADERTEST: = call { <4 x i32>, <4 x i1> } @llvm.uadd.with.overflow.v4i32(<4 x i32>
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: call { <4 x i32>, <4 x i1> } @llvm.uadd.with.overflow.v4i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
