#version 450 core

layout(location = 0) in float a;
layout(location = 1) in vec4 b0;


layout(location = 0) out vec4 frag_color;
void main()
{
    frag_color = vec4(mod(b0.x,a));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call reassoc nnan nsz arcp contract float (...) @llpc.call.fmod.f32(float
; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: fdiv reassoc nnan nsz arcp contract float
; SHADERTEST: fmul reassoc nnan nsz arcp contract float
; SHADERTEST: call reassoc nnan nsz arcp contract float @llvm.floor.f32
; SHADERTEST: fsub reassoc nnan nsz arcp contract float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
