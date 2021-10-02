#version 450 core

layout(location = 0) in vec4 a;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = fract(a);
    fv.x = fract(1.5);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.fract.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[^ ]+}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fract.f32(float %{{[^) ]+}})
; SHADERTEST: %{{[^ ]+}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fract.f32(float %{{[^) ]+}})
; SHADERTEST: %{{[^ ]+}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fract.f32(float %{{[^) ]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
