#version 450 core

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
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results

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
