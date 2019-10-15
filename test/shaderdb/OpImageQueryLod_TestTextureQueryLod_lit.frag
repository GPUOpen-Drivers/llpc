#version 450

layout(set = 0, binding = 0) uniform sampler1D          samp1D;
layout(set = 1, binding = 0) uniform sampler2D          samp2D[4];
layout(set = 0, binding = 1) uniform sampler2DShadow    samp2DShadow;
layout(set = 2, binding = 0) uniform samplerCubeArray   sampCubeArray[4];
layout(set = 3, binding = 0) uniform texture3D          tex3D;
layout(set = 3, binding = 1) uniform sampler            samp;

layout(set = 4, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 f2 = textureQueryLod(samp1D, 1.0);
    f2 += textureQueryLod(samp2D[index], vec2(0.5));
    f2 += textureQueryLod(samp2DShadow, vec2(0.4));
    f2 += textureQueryLod(sampCubeArray[index], vec3(0.6));
    f2 += textureQueryLod(sampler3D(tex3D, samp), vec3(0.7));

    fragColor = vec4(f2, f2);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 0, i32 0)
; SHADERTEST: call reassoc nnan nsz arcp contract <2 x float> (...) @llpc.call.image.get.lod.v2f32(i32 0, i32 0, {{.*}}, {{.*}}, float 1.000000e+00)
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 1, i32 0)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 1, i32 0)
; SHADERTEST: call reassoc nnan nsz arcp contract <2 x float> (...) @llpc.call.image.get.lod.v2f32(i32 1, i32 0, {{.*}}, {{.*}}, <2 x float> <float 5.000000e-01, float 5.000000e-01>)
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 1)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 0, i32 1)
; SHADERTEST: call reassoc nnan nsz arcp contract <2 x float> (...) @llpc.call.image.get.lod.v2f32(i32 1, i32 0, {{.*}}, {{.*}}, <2 x float> <float 0x3FD99999A0000000, float 0x3FD99999A0000000>)
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 2, i32 0)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 2, i32 0)
; SHADERTEST: call reassoc nnan nsz arcp contract <2 x float> (...) @llpc.call.image.get.lod.v2f32(i32 8, i32 0, {{.*}}, {{.*}}, <3 x float> <float 0x3FE3333340000000, float 0x3FE3333340000000, float 0x3FE3333340000000>)
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 3, i32 0)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 3, i32 1)
; SHADERTEST: call reassoc nnan nsz arcp contract <2 x float> (...) @llpc.call.image.get.lod.v2f32(i32 2, i32 0, {{.*}}, {{.*}}, <3 x float> <float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000>)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <2 x float> @llvm.amdgcn.image.getlod.1d.v2f32.f32(i32 3, float 1.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <2 x float> @llvm.amdgcn.image.getlod.2d.v2f32.f32(i32 3, float 5.000000e-01, float 5.000000e-01,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <2 x float> @llvm.amdgcn.image.getlod.2d.v2f32.f32(i32 3, float 0x3FD99999A0000000, float 0x3FD99999A0000000,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <2 x float> @llvm.amdgcn.image.getlod.cube.v2f32.f32(i32 3,{{.*}},{{.*}},{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <2 x float> @llvm.amdgcn.image.getlod.3d.v2f32.f32(i32 3, float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
