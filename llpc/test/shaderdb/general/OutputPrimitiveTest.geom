// This test case checks whether the emit instruction and the primitive type are well-handled when there is no output
// of the geometry shader.
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-COUNT-2: call void (...) @lgc.create.emit.vertex(i32 0)
; SHADERTEST-LABEL: _amdgpu_gs_main:
; SHADERTEST-COUNT-2: s_sendmsg sendmsg(MSG_GS, GS_OP_EMIT, 0)
; SHADERTEST-LABEL: PalMetadata
; SHADERTEST-LABEL: .graphics_registers:
; SHADERTEST:         .vgt_gs_out_prim_type:
; SHADERTEST:         .outprim_type:   LineStrip
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450
layout(lines) in;
layout(max_vertices = 2, line_strip) out;

void main()
{
    EmitVertex();
    EmitVertex();
    EndPrimitive();
}
