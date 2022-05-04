#version 450 core

layout(std430, binding = 0, row_major) buffer Block
{
    vec3   f;
    mat2x3 m2x3;
} block;

void main()
{
    mat2x3 m2x3;
    m2x3[0] = vec3(0.1, 0.2, 0.3);
    m2x3[1] = block.f;

    block.m2x3 = m2x3;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1036831949, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1045220557, <4 x i32> {{%[^,]+}}, i32 24, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1050253722, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 20, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 28, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 36, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
