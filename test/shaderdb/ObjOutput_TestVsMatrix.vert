#version 450 core

layout(location = 3) out mat4 m4;

layout(binding = 0) uniform Uniforms
{
    int i;
};

void main()
{
    m4[i] = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.exp.f32
; SHADERTEST: call void @llvm.amdgcn.exp.f32
; SHADERTEST: call void @llvm.amdgcn.exp.f32
; SHADERTEST: call void @llvm.amdgcn.exp.f32
; SHADERTEST: call void @llvm.amdgcn.exp.f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
