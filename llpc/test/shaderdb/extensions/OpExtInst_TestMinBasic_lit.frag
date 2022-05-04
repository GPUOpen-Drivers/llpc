#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;


layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fmin = min(a,b);
    ivec4 imin = min(ivec4(a), ivec4(b));
    uvec4 umin = min(uvec4(a), uvec4(b));
    frag_color = fmin + vec4(imin) + vec4(umin);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.minnum.v4f32(<4 x float>
; SHADERTEST: = call <4 x i32> @llvm.smin.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}})
; SHADERTEST: = call <4 x i32> @llvm.umin.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
