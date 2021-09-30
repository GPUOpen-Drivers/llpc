#version 450 core

layout(std140, binding = 0) uniform Block
{
    mat4 m4;
    int  i;
} block;

void main()
{
    int i = block.i;
    gl_Position = block.m4[i] + block.m4[1];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[0-9]*}}, i32 64, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 %{{[0-9a-z.]*}}, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 16, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
