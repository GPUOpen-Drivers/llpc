#version 450
#extension GL_ARB_sparse_texture_clamp : enable

layout(set = 0, binding = 0) uniform sampler1D          samp1D[4];
layout(set = 1, binding = 0) uniform sampler2D          samp2D;
layout(set = 2, binding = 0) uniform sampler3D          samp3D;
layout(set = 3, binding = 0) uniform samplerCube        sampCube;
layout(set = 4, binding = 0) uniform sampler1DArray     samp1DArray;
layout(set = 5, binding = 0) uniform sampler2DArray     samp2DArray;
layout(set = 6, binding = 0) uniform samplerCubeArray   sampCubeArray;

layout(set = 7, binding = 0) uniform Uniforms
{
    int   index;
    float lodClamp;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0);

    fragColor += textureGradOffsetClampARB(samp1D[index], 0.1, 0.2, 0.3, 2, lodClamp);

    fragColor += textureGradOffsetClampARB(samp2D, vec2(0.1), vec2(0.2), vec2(0.3), ivec2(2), lodClamp);

    fragColor += textureGradOffsetClampARB(samp3D, vec3(0.1), vec3(0.2), vec3(0.3), ivec3(2), lodClamp);

    fragColor += textureGradOffsetClampARB(samp1DArray, vec2(0.1), 0.2, 0.3, 2, lodClamp);

    fragColor += textureGradOffsetClampARB(samp2DArray, vec3(0.1), vec2(0.2), vec2(0.3), ivec2(2), lodClamp);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 384, {{.*}}, {{.*}}, i32 409, float 0x3FB99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, {{.*}}, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 409, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, <2 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000>, <2 x float> <float 0x3FD3333340000000, float 0x3FD3333340000000>, {{.*}}, <2 x i32> <i32 2, i32 2>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 2, i32 512, {{.*}}, {{.*}}, i32 409, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, <3 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000>, <3 x float> <float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FD3333340000000>, {{.*}}, <3 x i32> <i32 2, i32 2, i32 2>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 4, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 409, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 0x3FC99999A0000000, float 0x3FD3333340000000, {{.*}}, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 5, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 409, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, <2 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000>, <2 x float> <float 0x3FD3333340000000, float 0x3FD3333340000000>, {{.*}}, <2 x i32> <i32 2, i32 2>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 384, {{.*}}, {{.*}}, i32 409, float 0x3FB99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, {{.*}}, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 409, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, <2 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000>, <2 x float> <float 0x3FD3333340000000, float 0x3FD3333340000000>, {{.*}}, <2 x i32> <i32 2, i32 2>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 2, i32 512, {{.*}}, {{.*}}, i32 409, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, <3 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000>, <3 x float> <float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FD3333340000000>, {{.*}}, <3 x i32> <i32 2, i32 2, i32 2>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 4, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 409, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 0x3FC99999A0000000, float 0x3FD3333340000000, {{.*}}, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 5, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 409, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, <2 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000>, <2 x float> <float 0x3FD3333340000000, float 0x3FD3333340000000>, {{.*}}, <2 x i32> <i32 2, i32 2>)

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.readfirstlane
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.o.1d.v4f32.f32.f32({{.*}}, i32 2, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.o.2d.v4f32.f32.f32({{.*}}, i32 514, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.o.3d.v4f32.f32.f32({{.*}}, i32 131586, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.o.1darray.v4f32.f32.f32({{.*}}, i32 2, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float 0.000000e+00, {{.*}})
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{.*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.cl.o.2darray.v4f32.f32.f32({{.*}}, i32 514, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
