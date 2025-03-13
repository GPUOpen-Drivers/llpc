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
    vec4 f4_1;
    float f1;
    float f2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4_0 = vec4(f2);
    f4_0 = mod(f4_0, f4_1);

    f4_0 += mod(f4_0, f1);

    fragColor = (f4_0.y > 0.0) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.fmod.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: fdiv reassoc nnan nsz arcp contract afn float
; SHADERTEST: call reassoc nnan nsz arcp contract afn float @llvm.floor.f32
; SHADERTEST: fsub reassoc nnan nsz arcp contract afn float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
