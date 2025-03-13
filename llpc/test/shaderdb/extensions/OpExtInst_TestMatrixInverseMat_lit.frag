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
    mat2 m2_1;
    mat3 m3_1;
    mat4 m4_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    mat2 m2_0 = inverse(m2_1);

    mat3 m3_0 = inverse(m3_1);

    mat4 m4_0 = inverse(m4_1);

    fragColor = ((m2_0[0] != m2_0[1]) || (m3_0[0] != m3_0[1]) || (m4_0[0] != m4_0[1])) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call {{.*}}[2 x <2 x float>] (...) @lgc.create.matrix.inverse.a2v2f32([2 x <2 x float>] %
; SHADERTEST: = call {{.*}}[3 x <3 x float>] (...) @lgc.create.matrix.inverse.a3v3f32([3 x <3 x float>] %
; SHADERTEST: = call {{.*}}[4 x <4 x float>] (...) @lgc.create.matrix.inverse.a4v4f32([4 x <4 x float>] %
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
