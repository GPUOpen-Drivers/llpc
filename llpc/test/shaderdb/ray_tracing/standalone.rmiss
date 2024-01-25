// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: _miss1:
; SHADERTEST: s_buffer_load_dwordx2 s{{\[}}[[address_lo:[0-9]+]]:[[address_hi:[0-9]+]]{{\]}}, s[0:3], 0x14
; SHADERTEST: s_buffer_load_dword [[stride:s[0-9]+]], s[0:3], 0x1c
; add size of shader ids to base address to get the start of the shader record buffer
; SHADERTEST: s_add_u32 s[[shader_record_lo:[0-9]+]], s[[address_lo]], [[shader_ids_size:32]]
; SHADERTEST: s_addc_u32 s[[shader_record_hi:[0-9]+]], s[[address_hi]], 0
; SHADERTEST-NOT: v_mad_u64_u32 v{{[[0-9]+:[0-9]+]}}, null, [[stride]], v4, 32
; SHADERTEST-NOT: global_load_dword v{{[0-9]}}, v{{[0-9]}}, s[[[address_lo]]:[[address_hi]]]
; SHADERTEST: buffer_load_dword v{{[0-9]+}}, v{{[0-9]+}}, s{{\[}}[[shader_record_lo]]:{{[0-9]+\]}}, {{[0-9]+}} idxen
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 460
#extension GL_EXT_ray_tracing : enable

layout(location = 0) rayPayloadInEXT vec3 hitValue;

layout(shaderRecordEXT, std430) buffer SBT {
  float b;
};

void main()
{
  hitValue = vec3(b, b, b);
}
