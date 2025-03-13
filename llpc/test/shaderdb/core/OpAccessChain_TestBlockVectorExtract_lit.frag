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


layout(set = 0, binding = 0) uniform DATA0
{
    vec3   f3;
    mat2x3 m2x3;
} data0;

layout(set = 1, binding = 1) buffer DATA1
{
    dvec4  d4;
    dmat4  dm4;
} data1;

layout(location = 0) out vec4 fragColor;

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

void main()
{
    float f1 = data0.f3[1];
    f1 += data0.m2x3[1][index];
    f1 += data0.m2x3[index][1];

    double d1 = data1.d4[index];
    d1 += data1.dm4[2][3];
    d1 += data1.dm4[index][index + 1];

    fragColor = vec4(float(d1), f1, f1, f1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results

; SHADERTEST: %[[COLUMN1:.*]] = type <{ [3 x float], [4 x i8] }>
; SHADERTEST: %[[COLUMN2:.*]] = type <{ [4 x double] }>

; SHADERTEST: getelementptr {{(inbounds )?}}(<{ [3 x float], [4 x i8], [2 x %[[COLUMN1]]] }>, ptr addrspace({{.*}}) @{{.*}}, i32 0, i32 0, i32 1
; SHADERTEST: getelementptr <{ [3 x float], [4 x i8], [2 x %[[COLUMN1]]] }>, ptr addrspace({{.*}}) @{{.*}}, i32 0, i32 2, i32 1, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: getelementptr <{ [3 x float], [4 x i8], [2 x %[[COLUMN1]]] }>, ptr addrspace({{.*}}) @{{.*}}, i32 0, i32 2, i32 %{{[0-9]*}}, i32 0, i32 1
; SHADERTEST: getelementptr <{ [4 x double], [4 x %[[COLUMN2]]] }>, ptr addrspace({{.*}}) @{{.*}}, i32 0, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: getelementptr {{(inbounds )?}}(<{ [4 x double], [4 x %[[COLUMN2]]] }>, ptr addrspace({{.*}}) @{{.*}}, i32 0, i32 1, i32 2, i32 0, i32 3
; SHADERTEST: getelementptr <{ [4 x double], [4 x %[[COLUMN2]]] }>, ptr addrspace({{.*}}) @{{.*}}, i32 0, i32 1, i32 %{{[0-9]*}}, i32 0, i32 %{{[0-9]*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
