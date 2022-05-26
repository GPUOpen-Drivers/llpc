#version 450 core

layout(location = 0) out int i1;

void main()
{
    i1 = gl_VertexIndex + gl_InstanceIndex;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST-DAG: call i32 @lgc.special.user.data.BaseInstance(i32 268435460)
; SHADERTEST-DAG: call i32 @lgc.shader.input.VertexId(i32 15)
; SHADERTEST-DAG: call i32 @lgc.special.user.data.BaseVertex(i32 268435459)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
