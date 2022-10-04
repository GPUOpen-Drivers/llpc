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
    vec4  nonConstVec;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0);

    fragColor += textureClampARB(samp1D[index], nonConstVec.x, lodClamp, 2.0);

    fragColor += textureClampARB(samp2D, nonConstVec.xy, lodClamp, 2.0);

    fragColor += textureClampARB(samp3D, nonConstVec.xyz, lodClamp, 2.0);

    fragColor += textureClampARB(sampCube, nonConstVec.xyz, lodClamp, 2.0);

    fragColor += textureClampARB(samp1DArray, nonConstVec.xy, lodClamp, 2.0);

    fragColor += textureClampARB(samp2DArray, nonConstVec.xyz, lodClamp, 2.0);

    fragColor += textureClampARB(sampCubeArray, nonConstVec.xyzw, lodClamp, 2.0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 384, {{.*}}, {{.*}}, i32 193, float %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 193, <2 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 2, i32 512, {{.*}}, {{.*}}, i32 193, <3 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 3, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 3, i32 512, {{.*}}, {{.*}}, i32 193, <3 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 4, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 193, <2 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 5, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 193, <3 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 6, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 8, i32 512, {{.*}}, {{.*}}, i32 193, <4 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 384, {{.*}}, {{.*}}, i32 193, float %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 193, <2 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 2, i32 512, {{.*}}, {{.*}}, i32 193, <3 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 3, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 3, i32 512, {{.*}}, {{.*}}, i32 193, <3 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 4, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 4, i32 512, {{.*}}, {{.*}}, i32 193, <2 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 5, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 193, <3 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 6, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 8, i32 512, {{.*}}, {{.*}}, i32 193, <4 x float> %{{[0-9]*}}, float 2.000000e+00, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.readfirstlane
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.cl.1d.v4f32.f32.f32({{.*}}, float 2.000000e+00, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.cl.2d.v4f32.f32.f32({{.*}}, float 2.000000e+00, float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.cl.3d.v4f32.f32.f32({{.*}}, float 2.000000e+00, float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubesc(float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}})
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubetc(float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}})
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubema(float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}})
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubeid(float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[.i0-9]*}})
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.cl.cube.v4f32.f32.f32({{.*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.cl.1darray.v4f32.f32.f32({{.*}}, float 2.000000e+00, float %{{[.i0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.cl.2darray.v4f32.f32.f32({{.*}}, float 2.000000e+00, float %{{[.i0-9]*}}, float %{{[.i0-9]*}}, float %{{[0-9]*}}, {{.*}})
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.cl.cube.v4f32.f32.f32({{.*}}, float 2.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
