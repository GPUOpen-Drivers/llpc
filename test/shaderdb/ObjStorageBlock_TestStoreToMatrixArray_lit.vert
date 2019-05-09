#version 450 core

layout(std430, binding = 0) buffer Block
{
    int  i;
    mat4 m4[16];
} block;

void main()
{
    int i = block.i;
    block.m4[3][3] = vec4(2.0);
    block.m4[i][i] = vec4(3.0);

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 2.000000e+00, <4 x i32> {{%[^,]+}}, i32 256, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 2.000000e+00, <4 x i32> {{%[^,]+}}, i32 260, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 2.000000e+00, <4 x i32> {{%[^,]+}}, i32 264, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 2.000000e+00, <4 x i32> {{%[^,]+}}, i32 268, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
