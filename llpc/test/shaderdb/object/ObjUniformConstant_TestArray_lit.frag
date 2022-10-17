#version 450

layout(binding = 0) uniform Uniforms
{
    vec4 ca[3];
    int i;
};

layout(location = 0) out vec4 o1;

void main()
{
    o1 = ca[i];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.s.buffer.load.i32(<4 x i32> %{{[0-9]*}}, i32 48, i32 0), !invariant.load
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 %{{[0-9a-z.]*}}, i32 0), !invariant.load

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
