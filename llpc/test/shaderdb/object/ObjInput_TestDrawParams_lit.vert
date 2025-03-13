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


#extension GL_ARB_shader_draw_parameters: enable

layout(binding = 0) uniform Block
{
    vec4 pos[2][4];
} block;

void main()
{
    if ((gl_BaseVertexARB > 0) || (gl_BaseInstanceARB > 0))
        gl_Position = block.pos[gl_VertexIndex % 2][gl_DrawIDARB % 4];
    else
        gl_Position = block.pos[gl_InstanceIndex % 2][gl_DrawIDARB % 4];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results

; SHADERTEST-DAG: [[BASEINSTANCE:%.*]] = call i32 @lgc.special.user.data.BaseInstance(
; SHADERTEST-DAG: [[INSTANCEID:%.*]] = call i32 @lgc.shader.input.InstanceId(
; SHADERTEST-DAG: %InstanceIndex = add i32 [[BASEINSTANCE]], [[INSTANCEID]]
; SHADERTEST-DAG: call i32 @lgc.special.user.data.DrawIndex(
; SHADERTEST-DAG: [[BASEVERTEX:%.*]] = call i32 @lgc.special.user.data.BaseVertex(
; SHADERTEST-DAG: [[VERTEXID:%.*]] = call i32 @lgc.shader.input.VertexId(
; SHADERTEST-DAG: %VertexIndex = add i32 [[BASEVERTEX]], [[VERTEXID]]
; SHADERTEST-DAG: call i32 @lgc.special.user.data.BaseVertex(
; SHADERTEST-DAG: call i32 @lgc.special.user.data.BaseInstance(
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
