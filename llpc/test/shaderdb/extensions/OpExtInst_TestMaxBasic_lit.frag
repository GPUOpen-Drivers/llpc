#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;


layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fmax = max(a,b);
    ivec4 imax = max(ivec4(a), ivec4(b));
    uvec4 umax = max(uvec4(a), uvec4(b));
    frag_color = fmax + vec4(imax) + vec4(umax);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.maxnum.v4f32(<4 x float>
; SHADERTEST: = call <4 x i32> @llvm.smax.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}})
; SHADERTEST: = call <4 x i32> @llvm.umax.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
