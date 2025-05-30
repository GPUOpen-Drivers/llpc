#version 450
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


layout(binding = 0) uniform Uniforms
{
    int i1_1;
    ivec3 i3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1_0 = sign(i1_1);

    ivec3 i3_0 = sign(i3_1);

    fragColor = ((i1_0 != i3_0.x)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call i32 (...) @lgc.create.ssign.i32(i32
; SHADERTEST: = call <3 x i32> (...) @lgc.create.ssign.v3i32(<3 x i32>
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: = icmp {{slt i32 %.*, 1|sgt i32 %.*, 0}}
; SHADERTEST: = select i1 %{{.*}}, {{i32 %.*, i32 1|i32 1, i32 %.*}}
; SHADERTEST: = icmp {{sgt i32 %.*, -1|sge i32 %.*, 0}}
; SHADERTEST: = select i1 %{{.*}}, i32 %{{.*}}, i32 -1
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
