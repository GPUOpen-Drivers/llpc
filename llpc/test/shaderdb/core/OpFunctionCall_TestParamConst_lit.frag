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


const bool g_envCond = bool(false);

vec2 foo(vec2 coord, const bool cond)
{
    if (cond)
    {
        return vec2(coord.x, 1.0 - coord.y);
    }
    else
    {
        return coord;
    }
}

layout(location = 0) in vec2 envCoord;
layout(location = 0) out vec2 color;

void main()
{
    color = foo(envCoord, g_envCond);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -trim-debug-info=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} <2 x float> @"foo({{.*}};{{.*}};"({{.*}}, i1 false)
; SHADERTEST: define internal {{.*}} <2 x float> @"foo({{.*}}"(ptr {{.*}} %coord, i1 %cond)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
