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


#extension GL_AMD_shader_explicit_vertex_parameter: enable

layout(location = 0) out vec2 fOut;

void main()
{
    fOut = gl_BaryCoordNoPerspAMD;
    fOut += gl_BaryCoordNoPerspCentroidAMD;
    fOut += gl_BaryCoordNoPerspSampleAMD;
    fOut += gl_BaryCoordSmoothAMD;
    fOut += gl_BaryCoordSmoothCentroidAMD;
    fOut += gl_BaryCoordSmoothSampleAMD;
    fOut += gl_BaryCoordPullModelAMD.xy + gl_BaryCoordPullModelAMD.yz;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-DAG: call <3 x float> @lgc.input.import.builtin.BaryCoordPullModel.v3f32.i32
; SHADERTEST-DAG: call <2 x float> @lgc.input.import.builtin.BaryCoordSmoothSample.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @lgc.input.import.builtin.BaryCoordSmoothCentroid.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @lgc.input.import.builtin.BaryCoordSmooth.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @lgc.input.import.builtin.BaryCoordNoPerspSample.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @lgc.input.import.builtin.BaryCoordNoPerspCentroid.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @lgc.input.import.builtin.BaryCoordNoPersp.v2f32.i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
