// This test case checks that the unlinked shader add all of the appropriate relocations for buffer descriptors.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -o %t.elf %gfxip %s -v | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: _amdgpu_vs_main_fetchless:
; SHADERTEST: s_cmp_eq_u32 dusespill_0_0@abs32@lo, 0
; SHADERTEST: s_cselect_b32 s[[RELOCCOND:[0-9]+]], s[[ds0:[0-9]*]], s[[spill:[0-9]*]]
; SHADERTEST: s_mov_b32 s[[RELOREG:[0-9]+]], doff_0_0_b@abs32@lo
; SHADERTEST: s_load_dwordx2 s[{{.*}}:{{.*}}], s[{{.*}}:{{.*}}], s[[RELOREG]]
// register is reserved for the user data node holding descriptor set 0.
; SHADERTEST: SPI_SHADER_USER_DATA_VS_[[ds0]]                     0x0000000080000000
// register pointed to the user data spill table
; SHADERTEST: SPI_SHADER_USER_DATA_VS_[[spill]]                     0x0000000010000002
*/
// END_SHADERTEST

#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    vec4 proj;
} ubo;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = ubo.proj;
    fragColor = inColor;
}
