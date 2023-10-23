#version 450 core

struct S
{
    int  i;
    vec4 f4;
    mat4 m4;
};

layout(std140, binding = 0) uniform Block
{
    S s;
} block;

void main()
{
    S s = block.s;
    gl_Position = (s.i > 0) ? s.f4 : s.m4[s.i];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: [[TMP23:%.*]] = call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[0-9]*}}, i32 0, i32 0)
; SHADERTEST: freeze i32 [[TMP23]]
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 16, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
