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

layout(location = 1) out vec4 outColor[];

void main (void)
{
    outColor[gl_InvocationID] = gl_in[gl_InvocationID].gl_Position;
    outColor[gl_InvocationID].x += gl_in[gl_InvocationID].gl_PointSize;
    outColor[gl_InvocationID].y += gl_in[gl_InvocationID].gl_ClipDistance[2];
    outColor[gl_InvocationID].z += gl_in[gl_InvocationID].gl_CullDistance[3];

    int value = gl_PatchVerticesIn + gl_PrimitiveID;
    outColor[gl_InvocationID].w += float(value);
    outColor[gl_InvocationID].w += gl_in[gl_InvocationID].gl_Position.z;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call <4 x float> @lgc.input.import.builtin.Position.v4f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.builtin.PointSize.f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.builtin.ClipDistance.f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.builtin.CullDistance.f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.PatchVertices{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.PrimitiveId{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.builtin.Position.f32{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
