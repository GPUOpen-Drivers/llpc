#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) out int i1;

void main()
{
    i1 = gl_VertexIndex + gl_InstanceIndex;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST-DAG: call i32 @lgc.special.user.data.BaseInstance(i32 268435460)
; SHADERTEST-DAG: call i32 @lgc.shader.input.VertexId(i32 1{{[0-9]*}})
; SHADERTEST-DAG: call i32 @lgc.special.user.data.BaseVertex(i32 268435459)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
