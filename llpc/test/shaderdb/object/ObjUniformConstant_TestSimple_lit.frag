#version 450

layout(binding = 0) uniform Uniforms
{
    vec4 c0;
    vec4 c1;
};

layout(location = 0) out vec4 o1;

void main()
{
    o1 = c0 + c1;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: load <4 x float>,
; SHADERTEST: load <4 x float>,

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 0, i32 0), !invariant.load
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 16, i32 0), !invariant.load

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
