#version 450

struct str
{
    float f;
};

layout(set = 0, binding = 0, std430) buffer BB
{
   layout(offset = 128) vec4 m1;
   layout(offset = 256) str m2;
   layout(offset = 512) vec2 m3;
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
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC.*}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> %{{.*}}, <4 x i32> %{{[0-9]*}}, i32 128
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 256
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2i32(<2 x i32> %{{.*}}, <4 x i32> %{{[0-9]*}}, i32 512

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
