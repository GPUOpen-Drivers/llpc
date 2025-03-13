#version 450 core
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


layout(location = 2) in dvec4 d4;
layout(location = 4) in dvec3 d3[2];
layout(location = 8) in dmat4 dm4;

layout(binding = 0) uniform Uniforms
{
    int i;
};

void main()
{
    dvec4 d = d4;
    d += dvec4(d3[i], 1.0);
    d += dm4[i];

    gl_Position = vec4(d);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -auto-layout-desc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-DAG: call <4 x double> @lgc.load.vertex.input__v4f64{{.*}}
; SHADERTEST-DAG: call <4 x double> @lgc.load.vertex.input__v4f64{{.*}}
; SHADERTEST-DAG: call <4 x double> @lgc.load.vertex.input__v4f64{{.*}}
; SHADERTEST-DAG: call <4 x double> @lgc.load.vertex.input__v4f64{{.*}}
; SHADERTEST-DAG: call <4 x double> @lgc.load.vertex.input__v4f64{{.*}}
; SHADERTEST-DAG: call <3 x double> @lgc.load.vertex.input__v3f64{{.*}}
; SHADERTEST-DAG: call <3 x double> @lgc.load.vertex.input__v3f64{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST-COUNT-12: call i32 @llvm.amdgcn.struct.tbuffer.load.i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
