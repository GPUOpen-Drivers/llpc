#version 450 core

layout(location = 3) out mat4 m4[2];

layout(binding = 0) uniform Uniforms
{
    int i;
};

void main()
{
    m4[1][i] = vec4(1.0);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST-COUNT-9: call void @llvm.amdgcn.exp.f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
