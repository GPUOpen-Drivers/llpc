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


layout(location = 1) in Block
{
    flat int i1;
    centroid vec4 f4;
    noperspective sample mat4 m4;
} block;

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f = block.f4;
    f += block.m4[1];
    f += vec4(block.i1);

    fragColor = f;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-DAG: call i32 (...) @lgc.input.import.interpolated{{.*}}
; SHADERTEST-DAG: call <4 x float> (...) @lgc.input.import.interpolated__v4f32{{.*}}
; SHADERTEST-DAG: call <4 x float> (...) @lgc.input.import.interpolated__v4f32{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.mov
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
