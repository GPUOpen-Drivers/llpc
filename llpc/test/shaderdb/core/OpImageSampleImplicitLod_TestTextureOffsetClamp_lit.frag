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

    fragColor += textureOffsetClampARB(samp1D[index], 0.1, 2, lodClamp);

    fragColor += textureOffsetClampARB(samp2D, vec2(0.1), ivec2(2), lodClamp);

    fragColor += textureOffsetClampARB(samp3D, vec3(0.1), ivec3(2), lodClamp);

    fragColor += textureOffsetClampARB(samp1DArray, vec2(0.1), 2, lodClamp);

    fragColor += textureOffsetClampARB(samp2DArray, vec3(0.1), ivec2(2), lodClamp);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 384, {{.*}}, {{.*}}, i32 385, float 0x3FB99999A0000000, {{.*}}, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 385, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, {{.*}}, <2 x i32> <i32 2, i32 2>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 2, i32 512, {{.*}}, {{.*}}, i32 385, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, {{.*}}, <3 x i32> <i32 2, i32 2, i32 2>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 4, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 385, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, {{.*}}, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 5, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 385, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, {{.*}}, <2 x i32> <i32 2, i32 2>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 384, {{.*}}, {{.*}}, i32 385, float 0x3FB99999A0000000, {{.*}}, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 385, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, {{.*}}, <2 x i32> <i32 2, i32 2>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 2, i32 512, {{.*}}, {{.*}}, i32 385, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, {{.*}}, <3 x i32> <i32 2, i32 2, i32 2>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 4, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 385, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, {{.*}}, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 5, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 385, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, {{.*}}, <2 x i32> <i32 2, i32 2>)

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.readfirstlane
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.cl.o.1d.v4f32.f32({{.*}}, i32 2, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.cl.o.2d.v4f32.f32({{.*}}, i32 514, float 0x3FB99999A0000000, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.cl.o.3d.v4f32.f32({{.*}}, i32 131586, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.cl.o.1darray.v4f32.f32({{.*}}, i32 2, float 0x3FB99999A0000000, float 0.000000e+00, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.cl.o.2darray.v4f32.f32({{.*}}, i32 514, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
