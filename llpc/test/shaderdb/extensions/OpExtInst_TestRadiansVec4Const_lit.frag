#version 450 core

layout(location = 0) in vec4 a;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = radians(a);
    fv.x = radians(1.5);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <4 x float> %{{.*}}, <float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000>
; SHADERTEST: store float 0x3F9ACEEA00000000, ptr addrspace(5) %{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <4 x float> %{{.*}}, <float {{undef|poison}}, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000>
; SHADERTEST: = insertelement <4 x float> %{{.*}}, float 0x3F9ACEEA00000000, i{{32|64}} 0
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: [[mul1:%.i[0-9]*]] = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: [[mul2:%.i[0-9]*]] = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: [[mul3:%.i[0-9]*]] = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: float 0x3F9ACEEA00000000, float [[mul1]], float [[mul2]], float [[mul3]]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
