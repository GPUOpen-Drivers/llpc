#version 450 core

layout(std140, binding = 0) uniform Block
{
    int  i;
    mat4 m4;
    int  j;
} block;

void main()
{
    int i = block.i;
    mat4 m4 = block.m4;
    m4[i] = vec4(2.0);
    gl_Position = m4[block.j];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 16, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 32, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 48, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 64, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
