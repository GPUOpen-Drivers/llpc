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
    float f1_1;
    vec3 f3_1, f3_2, f3_3;
    vec4 f4_1, f4_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 f3_0 = mix(f3_1, f3_2, f3_3);

    vec4 f4_0 = mix(f4_1, f4_2, f1_1);

    fragColor = (f3_0.y == f4_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fsub reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: = fsub reassoc nnan nsz arcp contract afn <4 x float>
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <4 x float>
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <4 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
