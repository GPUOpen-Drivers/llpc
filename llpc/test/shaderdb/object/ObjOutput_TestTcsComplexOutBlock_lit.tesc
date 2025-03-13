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


layout(vertices = 5) out;

struct S
{
    int     x;
    vec4    y;
    float   z[2];
};

layout(location = 2) out TheBlock
{
    S     blockS;
    float blockFa[3];
    S     blockSa[2];
    float blockF;
} tcBlock[];

void main(void)
{
    S block = { 1, vec4(2), { 0.5, 0.7} };
    tcBlock[gl_InvocationID].blockSa[1] = block;

    int i = gl_InvocationID;
    tcBlock[gl_InvocationID].blockSa[i].z[i + 1] = 5.0;

    gl_TessLevelInner[0] = 2.0;
    gl_TessLevelOuter[0] = 4.0;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.generic{{.*}}
; SHADERTEST: call void @lgc.output.export.generic{{.*}}.v4f32
; SHADERTEST: call void @lgc.output.export.generic{{.*}}.f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
