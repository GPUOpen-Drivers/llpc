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


layout(vertices = 3) out;

layout(location = 1) out dvec4 outData1[];
layout(location = 4) patch out float outData2[4];

void main (void)
{
    outData1[gl_InvocationID] = dvec4(gl_in[gl_InvocationID].gl_Position);
    float f[4] = { 1.0, 2.0, 3.0, 4.0 };
    outData2 = f;
    f[0] += float(gl_PrimitiveID);
    outData2[gl_InvocationID] = f[0];

    barrier();

    outData1[gl_InvocationID].x += double(gl_InvocationID);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v4f64
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f32
; SHADERTEST: call double @lgc.output.import.generic__f64{{.*}}
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f64
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
