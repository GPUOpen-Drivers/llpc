#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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


layout(triangles) in;

struct S
{
    int     x;
    vec4    y;
    float   z[2];
};

layout(location = 2) in TheBlock
{
    S     blockS;
    float blockFa[3];
    S     blockSa[2];
    float blockF;
} teBlock[];

layout(location = 0) out float f;

layout(binding = 0) uniform Uniforms
{
    int i;
};

void main(void)
{
    S block = teBlock[2].blockSa[1];

    f = teBlock[1].blockSa[i].z[i + 1];
    f += block.z[1] + block.y[0];
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-DAG: call float @lgc.input.import.generic__f32{{.*}}
; SHADERTEST-DAG: call float @lgc.input.import.generic{{.*}}
; SHADERTEST-DAG: call <4 x float> @lgc.input.import.generic__v4f32{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
