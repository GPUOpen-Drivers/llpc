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


layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    uvec4 u4 = uvec4(5);
    uint u1 = 90;
    u4 *= u1;

    fragColor = (u4 == uvec4(6)) ? vec4(1.0) : color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{.*}} = insertelement <4 x i32> poison, i32 %{{.*}}, i32 0
; SHADERTEST: %{{.*}} = insertelement <4 x i32> %{{.*}}, i32 %{{.*}}, i32 1
; SHADERTEST: %{{.*}} = insertelement <4 x i32> %{{.*}}, i32 %{{.*}}, i32 2
; SHADERTEST: %{{.*}} = insertelement <4 x i32> %{{.*}}, i32 %{{.*}}, i32 3
; SHADERTEST: %{{.*}} = mul <4 x i32> %{{.*}}, %{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
