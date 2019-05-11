#version 450 core

struct S
{
    int  i;
    vec4 f4;
    mat4 m4;
};

layout(std430, binding = 0) buffer Block
{
    S    s0;
    S    s1;
} block;

void main()
{
    block.s1 = block.s0;

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 96, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 112, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 128, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 144, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 160, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 176, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
