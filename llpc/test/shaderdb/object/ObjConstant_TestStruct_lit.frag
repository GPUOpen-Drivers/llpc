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
    vec3  f3[3];
    mat3  m3;
    bool  b1;
    ivec2 i2;
};

layout(binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    const DATA info = { { vec3(0.1), vec3(0.5), vec3(1.0) }, mat3(1.0), true, ivec2(5) };

    DATA data = info;

    fragColor = data.b1 ? vec4(data.f3[i].y) : vec4(data.m3[i].z);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: alloca <4 x float>,{{.*}} addrspace(5)
; SHADERTEST: alloca { [3 x <3 x float>], [3 x <3 x float>], i32, <2 x i32> },{{.*}} addrspace(5)
; SHADERTEST: store { [3 x <3 x float>], [3 x <3 x float>], i32, <2 x i32> } { [3 x <3 x float>] [<3 x float> {{(splat \(float 0x3FB99999A0000000\))|(<float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>)}}, <3 x float> {{(splat \(float 5\.000000e\-01\))|(<float 5\.000000e\-01, float 5\.000000e\-01, float 5\.000000e\-01>)}}, <3 x float> {{(splat \(float 1\.000000e\+00\))|(<float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00>)}}], [3 x <3 x float>] [<3 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00>, <3 x float> <float 0.000000e+00, float 1.000000e+00, float 0.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>], i32 1, <2 x i32> {{(splat \(i32 5\))|(<i32 5, i32 5>)}} }, ptr addrspace(5) %{{[0-9]*}}
; SHADERTEST: load i32, ptr addrspace(5) %{{[0-9]*}}
; SHADERTEST: trunc i32 %{{[0-9]*}} to i1
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v4f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
