#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;


layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fstep = step(a, b);
    frag_color = fstep + step(vec4(0.7), vec4(0.2));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract afn olt <4 x float>
; SHADERTEST: = select reassoc nnan nsz arcp contract afn <4 x i1> %{{.*}}, <4 x float> zeroinitializer, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
