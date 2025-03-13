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


struct DATA
{
    dmat2x3 dm2x3;
    vec3 f3;
};

layout(location = 0) out OUTPUT
{
    vec3 f3;
    int i1;
    DATA data;
} outBlock;

layout(binding = 0) uniform Uniforms
{
    int index;
};

void main()
{
    outBlock.data.dm2x3[index] = dvec3(0.5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: icmp eq i32 %{{[0-9]*}}, 1
; SHADERTEST: select i1 %{{[0-9]*}}, ptr addrspace({{.*}}) %{{.*}}, ptr addrspace({{.*}}) %{{.*}}
; SHADERTEST: store <3 x double> {{(splat \(double 5\.000000e\-01\))|(<double 5\.000000e\-01, double 5\.000000e\-01, double 5\.000000e\-01>)}}, ptr addrspace({{.*}}) %{{[0-9]*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
