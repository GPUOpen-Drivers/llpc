#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(std430, binding = 0) buffer Block
{
    vec4 f4;
    mat4 m4;
} block;

void main()
{
    vec4 f4 = block.f4;
    mat4 m4 = mat4(f4, f4, f4, f4);
    block.m4 = m4;

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 48, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 64, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
