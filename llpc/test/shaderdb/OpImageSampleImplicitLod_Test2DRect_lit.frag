#version 450 core

layout(set = 0, binding = 0) uniform sampler2DRect samp;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = texture(samp, vec2(1, 2));
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
; SHADERTEST: call {{.*}} @llpc.call.image.sample.v4f32(i32 1, i32 0, {{.*}}, {{.*}}, i32 1, <2 x float> <float 1.000000e+00, float 2.000000e+00>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0) 
; SHADERTEST: call {{.*}} @llpc.call.image.sample.v4f32(i32 1, i32 0, {{.*}}, {{.*}}, i32 1, <2 x float> <float 1.000000e+00, float 2.000000e+00>) 

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32({{.*}}, float 1.000000e+00, float 2.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
