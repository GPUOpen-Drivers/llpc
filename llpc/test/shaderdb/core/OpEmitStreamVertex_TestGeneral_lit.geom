#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 32, stream = 1) out;

layout(location = 0) in vec4 colorIn[];
layout(location = 6) out vec4 colorOut;

void main ( )
{
    for (int i = 0; i < gl_in.length(); i++)
    {
        colorOut = colorIn[i];
        EmitStreamVertex(1);
    }

    EndStreamPrimitive(1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{[a-zA-Z_]+}} void @EmitStreamVertex.i32(i32 1)
; SHADERTEST: call void (...) @lgc.create.end.primitive(i32 1)
; SHADERTEST-LABEL: {{^// LLPC.*}} patching results
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 290, i32 %gsWaveId)
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 290, i32 %gsWaveId)
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 290, i32 %gsWaveId)
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 274, i32 %gsWaveId)
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 immarg 3, i32 %gsWaveId)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
