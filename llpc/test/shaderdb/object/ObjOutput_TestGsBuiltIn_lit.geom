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
layout(triangle_strip, max_vertices = 16) out;

layout(location = 2) in vec4 inColor[];

void main()
{
    for (int i = 0; i < gl_in.length(); ++i)
    {
        gl_Position  = inColor[i];
        gl_PointSize = inColor[i].x;
        gl_ClipDistance[2] = inColor[i].y;
        gl_CullDistance[1] = inColor[i].z;

        gl_PrimitiveID = gl_PrimitiveIDIn;
        gl_Layer = gl_InvocationID;
        gl_ViewportIndex = gl_InvocationID;

        EmitVertex();
    }

    EndPrimitive();
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.builtin.PrimitiveId{{.*}}
; SHADERTEST: call void @lgc.output.export.builtin.Layer{{.*}}
; SHADERTEST: call void @lgc.output.export.builtin.ViewportIndex{{.*}}
; SHADERTEST: call void @lgc.output.export.builtin.Position{{.*}}.v4f32
; SHADERTEST: call void @lgc.output.export.builtin.PointSize{{.*}}f32
; SHADERTEST: call void @lgc.output.export.builtin.ClipDistance{{.*}}a3f32
; SHADERTEST: call void @lgc.output.export.builtin.CullDistance{{.*}}a2f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
