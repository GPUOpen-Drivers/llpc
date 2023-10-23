#version 450 core

layout(set = 0, binding = 0) uniform sampler1DArray samp;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = texture(samp, vec2(1, 2));
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 1, <2 x float> <float 1.000000e+00, float 2.000000e+00>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 1, <2 x float> <float 1.000000e+00, float 2.000000e+00>)

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.1darray.v4f32.f16(i32 15, half 0xH3C00, half 0xH4000, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
