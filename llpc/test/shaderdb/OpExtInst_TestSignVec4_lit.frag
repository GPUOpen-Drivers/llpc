#version 450 core

layout(location = 0) in float a;
layout(location = 1) in vec4 b0;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 b = sign(b0);
    frag_color = b;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x float> (...) @lgc.create.fsign.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract ogt float %{{.*}}, 0.000000e+00
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract ogt float %{{.*}}, 0.000000e+00
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract ogt float %{{.*}}, 0.000000e+00
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract ogt float %{{.*}}, 0.000000e+00
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract oge float %{{.*}}, 0.000000e+00
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract oge float %{{.*}}, 0.000000e+00
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract oge float %{{.*}}, 0.000000e+00
; SHADERTEST: = fcmp reassoc nnan nsz arcp contract oge float %{{.*}}, 0.000000e+00
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
