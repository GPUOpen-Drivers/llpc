#version 450 core

layout(std140, binding = 0, column_major) uniform Block
{
    vec3   f;
    layout(row_major) mat2x3 m0;
    mat2x3 m1;
} block;

layout(location = 0) out vec3 f;

void main()
{
    f  = block.f;
    f += block.m0[1];
    f += block.m1[0];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST-LABEL: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> %{{[^, ]+}}, i32 0, i32 0)
; SHADERTEST-LABEL: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 8, i32 0)
; SHADERTEST-LABEL: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 20, i32 0)
; SHADERTEST-LABEL: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 36, i32 0)
; SHADERTEST-LABEL: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 52, i32 0)
; SHADERTEST-LABEL: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> %{{[^, ]+}}, i32 64, i32 0)
; SHADERTEST-LABEL: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 72, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
