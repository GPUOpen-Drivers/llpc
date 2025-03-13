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


layout(set = 1, binding = 0) coherent buffer Buffer
{
    readonly vec4 f4;
    restrict vec2 f2;
    volatile writeonly float f1;
} buf;

void main()
{
    vec4 texel = vec4(0.0);

    texel += buf.f4;
    texel.xy += buf.f2;
    buf.f1 = texel.w;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: %{{[0-9]*}} = call ptr addrspace(7) @lgc.load.buffer.desc(i64 1, i32 0, i32 0,
; SHADERTEST: %{{[0-9]*}} = load atomic float, ptr addrspace(7) %{{[0-9]*}} unordered, align 4
; SHADERTEST: store atomic float %{{[0-9a-z.]*}}, ptr addrspace(7) %{{[0-9]*}} unordered, align 4

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
