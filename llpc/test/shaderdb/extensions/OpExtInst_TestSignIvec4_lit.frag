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


layout(location = 0) in vec4 a0;
layout(location = 1) in vec4 b0;
layout(location = 0) out vec4 color;
void main()
{
    ivec4 ua = ivec4(a0);
    color = vec4(sign(ua));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call <4 x i32> (...) @lgc.create.ssign.v4i32(<4 x i32>
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: = icmp {{slt i32 %.*, 1|sgt <4 x i32> %.*, zeroinitializer}}
; SHADERTEST-DAG: = select {{i1 %.*, i32 %.*, i32 1|<4 x i1> %.*, <4 x i32> ((splat \(i32 1\))|(<i32 1, i32 1, i32 1, i32 1>)), <4 x i32> %.*}}
; SHADERTEST-DAG: = icmp {{sgt i32 %.*, -1|sge <4 x i32> %.*, zeroinitializer}}
; SHADERTEST-DAG: = select {{i1 %.*, i32 %.*, i32 -1|<4 x i1> %.*, <4 x i32> %.*, <4 x i32> ((splat \(i32 -1\))|(<i32 -1, i32 -1, i32 -1, i32 -1>))}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
