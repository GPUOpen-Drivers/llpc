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


layout(location = 0) out vec4 f4;
#extension GL_EXT_multiview : enable

void main()
{
    vec4 f = vec4(0.0);

    f.x   += float(gl_SampleMaskIn[0]);
    f     += gl_FragCoord;
    f.x   += float(gl_FrontFacing);
    f.x   += gl_ClipDistance[3];
    f.x   += gl_CullDistance[2];
    f.xy  += gl_PointCoord;
    f.x   += float(gl_PrimitiveID);
    f.x   += float(gl_SampleID);
    f.x   += float(gl_SampleMaskIn[0]);
    f.x   += float(gl_Layer);
    f.x   += float(gl_ViewportIndex);
    f.x   += float(gl_HelperInvocation);
    f.x   += float(gl_ViewIndex);

    f4 = f;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.ViewIndex{{.*}}
; SHADERTEST-DAG: call i1 (...) @lgc.create.is.helper.invocation.i1{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.ViewportIndex{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.Layer{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.SampleId{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.builtin.PrimitiveId{{.*}}
; SHADERTEST-DAG: call <2 x float> @lgc.input.import.builtin.PointCoord.v2f32.i32
; SHADERTEST-DAG: call [3 x float] @lgc.input.import.builtin.CullDistance.a3f32.i32
; SHADERTEST-DAG: call [4 x float] @lgc.input.import.builtin.ClipDistance.a4f32.i32
; SHADERTEST-DAG: call i1 @lgc.input.import.builtin.FrontFacing{{.*}}
; SHADERTEST-DAG: call <4 x float> @lgc.input.import.builtin.FragCoord.v4f32.i32
; SHADERTEST-DAG: call [1 x i32] @lgc.input.import.builtin.SampleMask.a1i32.i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
