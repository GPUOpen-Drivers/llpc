#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = pow(a, b);
    fv.x = pow(2.0, 3.0) + pow(-2.0, 2.0);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.power.v4f32(<4 x float>
; SHADERTEST: store float 1.200000e+01, float addrspace(5)* %{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x float> @llvm.pow.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.pow.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.pow.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract float @llvm.pow.f32(float
; SHADERTEST-NOT: = call reassoc nnan nsz arcp contract float @llvm.pow.f32(float
; SHADERTEST: call void @llvm.amdgcn.exp.f32(i32 immarg 0, i32 immarg 15, float 1.200000e+01, float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
