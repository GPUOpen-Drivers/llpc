#version 450 core

layout(push_constant) uniform PCB
{
    vec4 m0;
    vec4 m1[16];
} g_pc;

void main()
{
    gl_Position = g_pc.m1[8];
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST:  [[V0:%.*]] = call {{.*}} @lgc.create.load.push.constants.ptr
; SHADERTEST:  [[V1:%.*]] = getelementptr {{.*}} addrspace(4) [[V0]], i64 0, i32 1, i64 8
; SHADERTEST:  load <4 x float>, ptr addrspace(4) [[V1]], align 16

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
