#version 450 core

layout(std430, binding = 0) buffer Block
{
    int  i1[12];
    vec4 f4[16];
    int  i;
} block;

void main()
{
    int i = block.i;
    block.i1[7] = 23;
    block.i1[i] = 45;
    block.f4[3] = vec4(2.0);
    block.f4[i] = vec4(3.0);

    gl_Position = vec4(0.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 0x36E7000000000000, <4 x i32> {{%[^,]+}}, i32 28, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 0x36F6800000000000, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 2.000000e+00, <4 x i32> {{%[^,]+}}, i32 96, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 2.000000e+00, <4 x i32> {{%[^,]+}}, i32 100, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 2.000000e+00, <4 x i32> {{%[^,]+}}, i32 104, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 2.000000e+00, <4 x i32> {{%[^,]+}}, i32 108, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 {{%[^,]+}}, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
