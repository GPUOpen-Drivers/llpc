#version 450 core

layout(std140, binding = 0) uniform Block
{
    int  i;
    vec4 f4[4];
    int  j;
} block;

void main()
{
    int i = block.i;
    vec4 f4[4] = block.f4;
    f4[i] = vec4(2.0);
    gl_Position = f4[block.j];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 16, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 32, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 48, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 64, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
