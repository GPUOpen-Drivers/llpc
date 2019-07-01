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
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: <4 x i32> @spirv.image.sample.i32.2D({{.*}}, <2 x float> <float 0.000000e+00, float 1.000000e+00>, {{.*}})
; SHADERTEST: <4 x i32> @spirv.image.sample.u32.2D({{.*}}, <2 x float> <float 0.000000e+00, float 1.000000e+00>, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @"llpc.call.get.sampler.desc.ptr
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr
; SHADERTEST: call <4 x i32> @llpc.image.sample.i32.2D{{.*}}({{.*}}, <2 x float> <float 0.000000e+00, float 1.000000e+00>, {{.*}})
; SHADERTEST: call {{.*}} @"llpc.call.get.sampler.desc.ptr
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr
; SHADERTEST: call <4 x i32> @llpc.image.sample.u32.2D{{.*}}({{.*}}, <2 x float> <float 0.000000e+00, float 1.000000e+00>, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32({{.*}}, float 0.000000e+00, float 1.000000e+00, {{.*}})
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32({{.*}}, float 0.000000e+00, float 1.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
