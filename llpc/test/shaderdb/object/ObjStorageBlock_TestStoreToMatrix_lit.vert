#version 450 core

layout(std430, binding = 0) buffer Block
{
    int  i;
    mat4 m4;
} block;

void main()
{
    int i = block.i;
    block.m4[1] = vec4(2.0);
    block.m4[i] = vec4(3.0);

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1073741824, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1073741824, <4 x i32> {{%[^,]+}}, i32 36, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1073741824, <4 x i32> {{%[^,]+}}, i32 40, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1073741824, <4 x i32> {{%[^,]+}}, i32 44, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1077936128, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1077936128, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1077936128, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1077936128, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
