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
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.power.v4f32(<4 x float>
; SHADERTEST: store float 1.200000e+01, ptr addrspace(5) %{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.pow.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: [[mul1:%.i[0-9]*]] = call reassoc nnan nsz arcp contract afn float @llvm.pow.f32(float
; SHADERTEST: [[mul2:%.i[0-9]*]] = call reassoc nnan nsz arcp contract afn float @llvm.pow.f32(float
; SHADERTEST: [[mul3:%.i[0-9]*]] = call reassoc nnan nsz arcp contract afn float @llvm.pow.f32(float
; SHADERTEST-NOT: = call reassoc nnan nsz arcp contract afn float @llvm.pow.f32(float
; SHADERTEST: [[val1:%[.0-9a-zA-Z]*]] = insertelement <4 x float> <float 1.200000e+01, float {{undef|poison}}, float {{undef|poison}}, float {{undef|poison}}>, float [[mul1]], i{{32|64}} 1
; SHADERTEST: [[val2:%[.0-9a-zA-Z]*]] = insertelement <4 x float> [[val1]], float [[mul2]], i{{32|64}} 2
; SHADERTEST: [[val3:%[.0-9a-zA-Z]*]] = insertelement <4 x float> [[val2]], float [[mul3]], i{{32|64}} 3
; SHADERTEST: [[ret:%[0-9]*]] = insertvalue { <4 x float> } {{undef|poison}}, <4 x float> [[val3]], 0
; SHADERTEST: ret { <4 x float> } [[ret]]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
