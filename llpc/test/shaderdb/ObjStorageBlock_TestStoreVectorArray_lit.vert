#version 450 core

layout(std430, binding = 0) buffer Block
{
    float f1;
    vec4  f4[2];
} block;

void main()
{
    float f1 = block.f1;
    vec4 f4[2];
    f4[0] = vec4(f1);
    f4[1] = vec4(1.0);
    block.f4 = f4;

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> %{{[^, ]+}}, <4 x i32> %{{[^, ]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> <i32 1065353216, i32 1065353216, i32 1065353216, i32 1065353216>, <4 x i32> %{{[^, ]+}}, i32 32, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
