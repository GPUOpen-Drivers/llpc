#version 450 core

layout(std140, binding = 0, row_major) uniform Block
{
    vec3   f;
    mat2x3 m2x3;
} block;

layout(location = 0) out vec3 f;

void main()
{
    mat2x3 m2x3 = block.m2x3;
    f = m2x3[1] + block.f;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 20, i32 0)
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 36, i32 0)
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 52, i32 0)
; SHADERTEST-DAG: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST-DAG: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 8, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
