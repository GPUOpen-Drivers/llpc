#version 450

struct str
{
    float f;
};

layout(set = 0, binding = 0, std430) buffer BB
{
   layout(align = 32) vec4 m1;
   layout(align = 64) str m2;
   layout(align = 256) vec2 m3;
};

layout(set = 1, binding = 0) uniform Uniforms
{
    vec4 u1;
    float u2;
    vec2 u3;
};

layout(location = 0) out vec4 o1;
layout(location = 1) out float o2;
layout(location = 2) out vec2 o3;

void main()
{
    m1 = u1;
    m2.f = u2;
    m3 = u3;

    o1 = m1;
    o2 = m2.f;
    o3 = m3;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC.*}} pipeline patching
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 16, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 64, i32 0, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> {{%[^,]+}}, i32 24, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2i32(<2 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 256, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 64, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
