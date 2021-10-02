#version 450 core

layout(std140, binding = 0) uniform Block
{
    int   i1;
    ivec2 i2;
    ivec3 i3;
    ivec4 i4;
} block;

void main()
{
    ivec4 i4 = block.i4;
    i4.xyz += block.i3;
    i4.xy  += block.i2;
    i4.x   += block.i1;

    gl_Position = vec4(i4);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[^, ]+}}, i32 32, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> %{{[^, ]+}}, i32 16, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 24, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.s.buffer.load.v2i32(<4 x i32> %{{[^, ]+}}, i32 8, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[^, ]+}}, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
