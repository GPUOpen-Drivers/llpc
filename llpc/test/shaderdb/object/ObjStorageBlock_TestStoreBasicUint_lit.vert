#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(std430, binding = 0) buffer Block
{
    uint  u1;
    uvec2 u2;
    uvec3 u3;
    uvec4 u4;
} block;

void main()
{
    block.u1 += 1;
    block.u2 += uvec2(2);
    block.u3 += uvec3(3);
    block.u4 += uvec4(4);

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.load.i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 8, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2i32{{(\.v4i32)?}}(<2 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 8, i32 0, i32 0)
; SHADERTEST: call <3 x i32> @llvm.amdgcn.raw.buffer.load.v3i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v3i32{{(\.v4i32)?}}(<3 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
