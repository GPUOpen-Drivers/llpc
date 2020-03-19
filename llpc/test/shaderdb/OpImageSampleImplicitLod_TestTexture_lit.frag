#version 450

layout(set = 0, binding = 0) uniform sampler1D  samp1D;
layout(set = 1, binding = 0) uniform sampler2D  samp2D[4];

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = texture(samp1D, 1.0, 0.4);
    f4 += texture(samp2D[index], vec2(0.5));

    fragColor = f4;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
; SHADERTEST: call {{.*}} @llpc.call.image.sample.v4f32(i32 0, i32 0, {{.*}}, {{.*}}, i32 65, float 1.000000e+00, float 0x3FD99999A0000000)
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 1, i32 0)
; SHADERTEST: call {{.*}} @llpc.call.image.sample.v4f32(i32 1, i32 0, {{.*}}, {{.*}}, i32 1, <2 x float> <float 5.000000e-01, float 5.000000e-01>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0) 
; SHADERTEST: call {{.*}} @llpc.call.image.sample.v4f32(i32 0, i32 0, {{.*}}, {{.*}}, i32 65, float 1.000000e+00, float 0x3FD99999A0000000) 
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 1, i32 0) 
; SHADERTEST: call {{.*}} @llpc.call.image.sample.v4f32(i32 1, i32 0, {{.*}}, {{.*}}, i32 1, <2 x float> <float 5.000000e-01, float 5.000000e-01>) 

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.1d.v4f32.f32.f32({{.*}}, float 0x3FD99999A0000000, float 1.000000e+00, {{.*}})
; SHADERTEST: call i32 @llvm.amdgcn.readfirstlane
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32({{.*}}, float 5.000000e-01, float 5.000000e-01, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
