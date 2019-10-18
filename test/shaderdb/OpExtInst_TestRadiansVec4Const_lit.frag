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
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract <4 x float> %{{.*}}, <float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000>
; SHADERTEST: store float 0x3F9ACEEA00000000, float addrspace(5)* %{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract <4 x float> %{{.*}}, <float undef, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000>
; SHADERTEST: = insertelement <4 x float> %{{.*}}, float 0x3F9ACEEA00000000, i32 0
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{.*}} = fmul reassoc nnan nsz arcp contract float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: %{{.*}} = fmul reassoc nnan nsz arcp contract float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: %{{.*}} = fmul reassoc nnan nsz arcp contract float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float 0x3F9ACEEA00000000, float %{{.*}}, float %{{.*}}, float %{{.*}}, i1 true, i1 true)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
