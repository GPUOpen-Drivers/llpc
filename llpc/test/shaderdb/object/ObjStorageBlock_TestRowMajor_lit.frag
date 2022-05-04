#version 450

layout(std430, row_major, set = 0, binding = 0) buffer BufferObject
{
    mat4 m4;
};

layout(location = 0) out vec4 output0;

void main()
{
    m4[0] = vec4(1);
    output0 = m4[0];
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1065353216, <4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1065353216, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1065353216, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 1065353216, <4 x i32> {{%[^,]+}}, i32 48, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
