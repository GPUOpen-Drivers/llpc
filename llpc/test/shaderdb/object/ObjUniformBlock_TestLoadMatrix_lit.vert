#version 450 core

layout(std140, binding = 0) uniform Block
{
    int  i;
    mat4 m4;
} block;

void main()
{
    int i = block.i;
    mat4 m4 = block.m4;
    m4[i] = vec4(2.0);
    gl_Position = m4[i];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.exp.f32(i32 immarg 12, i32 immarg 15, float 2.000000e+00, float 2.000000e+00, float 2.000000e+00, float 2.000000e+00, i1 immarg true, i1 immarg false)
; SHADERTEST: call void @llvm.amdgcn.exp.f32(i32 32, i32 0, float undef, float undef, float undef, float undef, i1 false, i1 false)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
