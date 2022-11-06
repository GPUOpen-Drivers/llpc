#version 450 core

struct Str
{
    float m0;
    float m1;
};

layout (push_constant) uniform UBO
{
    layout (offset = 16)    vec4 m0;
    layout (offset = 4)     Str  m1;
} g_pc;

void main()
{
    gl_Position = vec4(g_pc.m1.m1);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v -enable-opaque-pointers=true %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST:  [[V0:%.*]] = call {{.*}} @lgc.create.load.push.constants.ptr
; SHADERTEST:  [[V1:%.*]] = getelementptr {{.*}} addrspace(4) [[V0]], i64 0, i32 1, i32 1
; SHADERTEST:  load float, ptr addrspace(4) [[V1]], align 4


; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
