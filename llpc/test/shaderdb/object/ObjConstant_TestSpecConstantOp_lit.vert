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


layout(constant_id = 200) const float f1  = 3.1415926;
layout(constant_id = 201) const int   i1  = -10;
layout(constant_id = 202) const uint  u1  = 100;

const int i1_1  = i1 + 2;
const uint u1_1 = u1 % 5;

const ivec4 i4 = ivec4(20, 30, i1_1, i1_1);
const ivec2 i2 = i4.yx;

void main()
{
    vec4 pos = vec4(0.0);
    pos.y += float(i1_1);
    pos.z += float(u1_1);

    pos += vec4(i4);
    pos.xy += vec2(i2);

    gl_Position = pos;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: fadd reassoc nnan nsz arcp contract afn float %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST: fadd reassoc nnan nsz arcp contract afn <4 x float> %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST: fadd reassoc nnan nsz arcp contract afn <2 x float> %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.builtin.Position{{.*}}v4f32(i32 0, <4 x float> <float 5.000000e+01, float 4.200000e+01, float -8.000000e+00, float -8.000000e+00>)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
