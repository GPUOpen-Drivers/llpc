#version 450 core

layout(std140, binding = 0, row_major) uniform Block
{
    vec3   f;
    mat2x3 m2x3;
} block;

layout(location = 0) out vec3 f;

void main()
{
    f  = block.f;
    f += block.m2x3[1];
    f += vec3(block.m2x3[0][2]);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 20, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 36, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 52, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 48, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
