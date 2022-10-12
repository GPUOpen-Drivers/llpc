#version 450

layout(location = 0) flat in vec4 color[4];
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform Uniforms
{
    int index;
};

void main()
{
    float f1 = color[index][index + 1];
    fragColor[index] = f1;
    fragColor[1] = 0.4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: add i32 %{{[0-9]*}}, 1
; SHADERTEST: getelementptr [4 x <4 x float>], ptr addrspace({{.*}}) %{{.*}}, i32 0, i32 %{{[0-9]*}}, i32 %{{[0-9]*}}
; SHADERTEST: load i32, ptr addrspace({{.*}}) %{{[0-9]*}}

; SHADERTEST: %[[cmp1:.*]] = icmp eq i32 %{{[0-9]*}}, 1
; SHADERTEST: %[[select1:.*]] = select i1 %[[cmp1]], i32 4, i32 0
; SHADERTEST: %[[cmp2:.*]] = icmp eq i32 %{{[0-9]*}}, 2
; SHADERTEST: %[[select2:.*]] = select i1 %[[cmp2]], i32 8, i32 %[[select1]]
; SHADERTEST: %[[cmp3:.*]] = icmp eq i32 %{{[0-9]*}}, 3
; SHADERTEST: select i1 %[[cmp3]], i32 12, i32 %[[select2]]
; SHADERTEST: store float %{{[0-9]*}}, ptr addrspace({{.*}}) %{{[0-9]*}}
; SHADERTEST: store float 0x3FD99999A0000000, ptr addrspace({{.*}}) %{{.*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
