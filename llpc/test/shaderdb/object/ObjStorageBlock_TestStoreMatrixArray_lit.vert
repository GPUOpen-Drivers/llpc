#version 450 core

layout(std430, binding = 0) buffer Block
{
    vec4 f4;
    mat4 m4[2];
} block;

void main()
{
    vec4 f4 = block.f4;
    mat4 m4[2];
    m4[0] = mat4(vec4(1.0), vec4(1.0), f4, f4);
    m4[1] = mat4(f4, f4, vec4(0.0), vec4(0.0));
    block.m4 = m4;

    gl_Position = vec4(0.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> <i32 1065353216, i32 1065353216, i32 1065353216, i32 1065353216>, <4 x i32> %{{[0-9]*}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> <i32 1065353216, i32 1065353216, i32 1065353216, i32 1065353216>, <4 x i32> %{{[0-9]*}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 48, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 64, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 80, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 96, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> zeroinitializer, <4 x i32> %{{[0-9]*}}, i32 112, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> zeroinitializer, <4 x i32> %{{[0-9]*}}, i32 128, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
