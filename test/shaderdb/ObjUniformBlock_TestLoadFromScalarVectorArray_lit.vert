#version 450 core

layout(std140, binding = 0) uniform Block
{
    int  i1[12];
    vec4 f4[16];
    int  i;
} block;

void main()
{
    int i = block.i;
    i = block.i1[7] + block.i1[i];
    gl_Position = block.f4[3] + block.f4[i];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[0-9]*}}, i32 448, i32 0)
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[0-9]*}}, i32 112, i32 0)
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[0-9]*}}, i32 %{{[0-9]*}}, i32 0)
; SHADERTEST-DAG: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 240, i32 0)
; SHADERTEST-DAG: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 %{{[0-9]*}}, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
