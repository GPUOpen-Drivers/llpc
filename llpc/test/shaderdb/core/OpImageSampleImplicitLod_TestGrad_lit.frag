#version 450 core

layout(set = 0, binding = 0) uniform sampler2D samp;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = textureGrad(samp, inUV, vec2(0.1, 0.2), vec2(0.3, 0.4));
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 25, {{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FC99999A0000000>, <2 x float> <float 0x3FD3333340000000, float 0x3FD99999A0000000>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 25, {{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FC99999A0000000>, <2 x float> <float 0x3FD3333340000000, float 0x3FD99999A0000000>)

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.2d.v4f32.f32.f32({{.*}}, float 0x3FB99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FD99999A0000000, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
