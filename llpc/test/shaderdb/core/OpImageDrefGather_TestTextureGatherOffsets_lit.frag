#version 450

layout(set = 0, binding = 0) uniform sampler2DShadow      samp2DShadow;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow samp2DArrayShadow[4];
layout(set = 0, binding = 1) uniform sampler2DRectShadow  samp2DRectShadow;

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    const ivec2 i2[4] = { ivec2(1), ivec2(2), ivec2(3), ivec2(4) };

    vec4 f4 = textureGatherOffsets(samp2DShadow, vec2(0.1), 0.9, i2);
    f4 += textureGatherOffsets(samp2DArrayShadow[index], vec3(0.2), 0.8, i2);
    f4 += textureGatherOffsets(samp2DRectShadow, vec2(1.0), 0.7, i2);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results

; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 1, i32 1, i64 0, i32 1)
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 2, i32 2, i64 0, i32 1)
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 1, i32 1, i64 1, i32 0)
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 2, i32 2, i64 1, i32 0)
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 2, i32 2, i64 0, i32 0)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.gather.v4f32(i32 1, i32 512, {{.*}}, i32 801, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 0.000000e+00, [4 x <2 x i32>] [<2 x i32> <i32 1, i32 1>, <2 x i32> <i32 2, i32 2>, <2 x i32> <i32 3, i32 3>, <2 x i32> <i32 4, i32 4>], float 0x3FECCCCCC0000000)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.gather.v4f32(i32 5, i32 384, {{.*}}, i32 801, <3 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000>, float 0.000000e+00, [4 x <2 x i32>] [<2 x i32> <i32 1, i32 1>, <2 x i32> <i32 2, i32 2>, <2 x i32> <i32 3, i32 3>, <2 x i32> <i32 4, i32 4>], float 0x3FE99999A0000000)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.gather.v4f32(i32 9, i32 512, {{.*}}, i32 801, <2 x float> <float 1.000000e+00, float 1.000000e+00>, float 0.000000e+00, [4 x <2 x i32>] [<2 x i32> <i32 1, i32 1>, <2 x i32> <i32 2, i32 2>, <2 x i32> <i32 3, i32 3>, <2 x i32> <i32 4, i32 4>], float 0x3FE6666660000000)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32(i32 1, i32 257, float 0x3FECCCCCC0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, <8 x i32> %{{[-0-9A-Za0z_.]+}}, <4 x i32> %{{[-0-9A-Za0z_.]+}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32(i32 1, i32 514, float 0x3FECCCCCC0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, <8 x i32> %{{[-0-9A-Za0z_.]+}}, <4 x i32> %{{[-0-9A-Za0z_.]+}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32(i32 1, i32 771, float 0x3FECCCCCC0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, <8 x i32> %{{[-0-9A-Za0z_.]+}}, <4 x i32> %{{[-0-9A-Za0z_.]+}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32(i32 1, i32 1028, float 0x3FECCCCCC0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, <8 x i32> %{{[-0-9A-Za0z_.]+}}, <4 x i32> %{{[-0-9A-Za0z_.]+}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2darray.v4f32.f32(i32 1, i32 257, float 0x3FE99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2darray.v4f32.f32(i32 1, i32 514, float 0x3FE99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2darray.v4f32.f32(i32 1, i32 771, float 0x3FE99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2darray.v4f32.f32(i32 1, i32 1028, float 0x3FE99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f16(i32 1, i32 257, float 0x3FE6666660000000, half 0xH3C00, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 true, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f16(i32 1, i32 514, float 0x3FE6666660000000, half 0xH3C00, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 true, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f16(i32 1, i32 771, float 0x3FE6666660000000, half 0xH3C00, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 true, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f16(i32 1, i32 1028, float 0x3FE6666660000000, half 0xH3C00, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 true, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
