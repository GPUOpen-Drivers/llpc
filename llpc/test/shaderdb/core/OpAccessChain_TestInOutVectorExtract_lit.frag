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


layout(location = 0) flat in vec4 color[4];
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform Uniforms
{
    int index;
};

void main()
{
    float f1 = color[index][index + 1];
    fragColor[index] = f1;
    fragColor[1] = 0.4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: add i32 %{{[0-9]*}}, 1
; SHADERTEST: getelementptr [4 x <4 x float>], ptr addrspace({{.*}}) %{{.*}}, i32 0, i32 %{{[0-9]*}}, i32 %{{[0-9]*}}
; SHADERTEST: load i32, ptr addrspace({{.*}}) %{{[0-9]*}}

; SHADERTEST: %[[cmp1:.*]] = icmp eq i32 %{{[0-9]*}}, 1
; SHADERTEST: %[[select1:.*]] = select i1 %[[cmp1]], i32 4, i32 0
; SHADERTEST: %[[cmp2:.*]] = icmp eq i32 %{{[0-9]*}}, 2
; SHADERTEST: %[[select2:.*]] = select i1 %[[cmp2]], i32 8, i32 %[[select1]]
; SHADERTEST: %[[cmp3:.*]] = icmp eq i32 %{{[0-9]*}}, 3
; SHADERTEST: select i1 %[[cmp3]], i32 12, i32 %[[select2]]
; SHADERTEST: store float %{{[0-9]*}}, ptr addrspace({{.*}}) %{{[0-9]*}}
; SHADERTEST: store float 0x3FD99999A0000000, ptr addrspace({{.*}}) %{{.*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
