#version 450 core

layout(set = 0, binding = 0) uniform sampler2DMS samp;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    ivec2 iUV = ivec2(inUV);
    oColor = texelFetch(samp, iUV, 2);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -o %t.elf %gfxip %s && llvm-objdump --triple=amdgcn --mcpu=gfx900 -d -r %t.elf | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST: s_getpc_b64 s[0:1]
; SHADERTEST: s_cmp_eq_u32 0, 0
; SHADERTEST-NEXT: R_AMDGPU_ABS32 $shadowenabled
; SHADERTEST: s_cselect_b32 s[[highAddr:[0-9]*]], s1, 0
; SHADERTEST-NEXT: R_AMDGPU_ABS32 $shadowdesctable
; SHADERTEST: s_mov_b32 [[offset:s[0-9]*]], 0
; SHADERTEST-NEXT: R_AMDGPU_ABS32 doff_0_0_f
; SHADERTEST: s_load_dwordx8 s[{{[0-9]*:[0-9]*}}], s[{{[0-9]*}}:[[highAddr]]], [[offset]]
*/
// END_SHADERTEST
