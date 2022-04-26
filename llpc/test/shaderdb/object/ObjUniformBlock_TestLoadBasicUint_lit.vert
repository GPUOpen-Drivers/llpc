#version 450 core

layout(std140, binding = 0) uniform Block
{
    uint  u1;
    uvec2 u2;
    uvec3 u3;
    uvec4 u4;
} block;

void main()
{
    uvec4 u4 = block.u4;
    u4.xyz += block.u3;
    u4.xy  += block.u2;
    u4.x   += block.u1;

    gl_Position = vec4(u4);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 32, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> %{{[0-9]*}}, i32 16, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[0-9]*}}, i32 24, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> %{{[0-9]*}}, i32 8, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[0-9]*}}, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
