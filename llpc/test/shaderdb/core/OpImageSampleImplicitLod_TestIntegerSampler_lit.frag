#version 450 core

layout(set = 0, binding = 0) uniform isampler2D iSamp2D;
layout(set = 0, binding = 1) uniform usampler2D uSamp2D;
layout(location = 0) out ivec4 oColor1;
layout(location = 1) out uvec4 oColor2;

void main()
{
    oColor1 = texture(iSamp2D, vec2(0, 1));
    oColor2 = texture(uSamp2D, vec2(0, 1));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.sample.v4i32(i32 1, i32 516, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> <float 0.000000e+00, float 1.000000e+00>)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.sample.v4i32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> <float 0.000000e+00, float 1.000000e+00>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4i32(i32 1, i32 516, {{.*}}, {{.*}}, i32 1, <2 x float> <float 0.000000e+00, float 1.000000e+00>)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4i32(i32 1, i32 512, {{.*}}, {{.*}}, i32 1, <2 x float> <float 0.000000e+00, float 1.000000e+00>)

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.sample.2d.v4i32.f16(i32 15, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.sample.2d.v4i32.f16(i32 15, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
