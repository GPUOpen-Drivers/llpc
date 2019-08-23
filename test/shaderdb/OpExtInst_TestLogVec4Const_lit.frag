#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = log(a);
    fv.x = log(2.0);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
' SHADERTEST-LABEL: = call <4 x float> (...) @llpc.call.log.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
' SHADERTEST-LABEL: = call <4 x float> (...) @llpc.call.log.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: = call float @llvm.log2.f32(float
; SHADERTEST: = call float @llvm.log2.f32(float
; SHADERTEST: = call float @llvm.log2.f32(float
; SHADERTEST-NOT: = call float @llvm.log2.f32(float
; SHADERTEST-LABEL: {{^// LLPC}} final pipeline module info
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
