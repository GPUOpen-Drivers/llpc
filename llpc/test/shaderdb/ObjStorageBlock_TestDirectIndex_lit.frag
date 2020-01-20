#version 450

layout(std430, column_major, set = 0, binding = 1) buffer BufferObject
{
    uint ui;
    vec4 v4;
    vec4 v4_array[2];
} ssbo[2];

layout(location = 0) out vec4 output0;

void main()
{
    output0 = ssbo[0].v4_array[1];

    ssbo[1].ui = 0;
    ssbo[1].v4 = vec4(3);
    ssbo[1].v4_array[0] = vec4(4);
    ssbo[1].v4_array[1] = vec4(5);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 0.000000e+00, <4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 20, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 24, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 3.000000e+00, <4 x i32> {{%[^,]+}}, i32 28, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 4.000000e+00, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 4.000000e+00, <4 x i32> {{%[^,]+}}, i32 36, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 4.000000e+00, <4 x i32> {{%[^,]+}}, i32 40, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 4.000000e+00, <4 x i32> {{%[^,]+}}, i32 44, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 5.000000e+00, <4 x i32> {{%[^,]+}}, i32 48, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 5.000000e+00, <4 x i32> {{%[^,]+}}, i32 52, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 5.000000e+00, <4 x i32> {{%[^,]+}}, i32 56, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.f32(float 5.000000e+00, <4 x i32> {{%[^,]+}}, i32 60, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
