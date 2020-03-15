#version 450 core

layout(set = 0, binding = 0) uniform sampler2D samp;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = texture(samp, vec2(0, 0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.image.sample.v4f32(i32 1, i32 0, <8 x i32> %{{[-0-9A-Za0z_.]+}}, <4 x i32> %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0) 
; SHADERTEST: call {{.*}} @llpc.call.image.sample.v4f32(i32 1, i32 0, {{.*}}, {{.*}}, i32 1, <2 x float> zeroinitializer) 

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32({{.*}}, float 0.000000e+00, float 0.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
