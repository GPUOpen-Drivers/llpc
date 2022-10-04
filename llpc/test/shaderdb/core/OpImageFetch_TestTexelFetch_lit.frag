#version 450

layout(set = 0, binding = 0) uniform sampler1D samp1D;
layout(set = 1, binding = 0) uniform sampler2D samp2D[4];
layout(set = 0, binding = 1) uniform sampler2DRect samp2DRect;
layout(set = 0, binding = 2) uniform samplerBuffer sampBuffer;
layout(set = 0, binding = 3) uniform sampler2DMS samp2DMS[4];

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = texelFetch(samp1D, 2, 2);
    f4 += texelFetch(samp2D[index], ivec2(7), 8);
    f4 += texelFetch(samp2DRect, ivec2(3));
    f4 += texelFetch(sampBuffer, 5);
    f4 += texelFetch(samp2DMS[index], ivec2(6), 4);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 0, i32 1536, {{.*}}, i32 2, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 1, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 128, {{.*}}, <2 x i32> <i32 7, i32 7>, i32 8)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 1
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 1536, {{.*}}, <2 x i32> <i32 3, i32 3>)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 4, i32 4, i32 0, i32 2
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 0, i32 1536, {{.*}}, i32 5)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 3
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.with.fmask.v4f32(i32 6, i32 128, {{.*}}, {{.*}}, <2 x i32> <i32 6, i32 6>, i32 4)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.1d.v4f32.i32(i32 15, i32 2, i32 2,{{.*}}, i32 0, i32 0), !invariant.load
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.2d.v4f32.i32(i32 15, i32 7, i32 7, i32 8,{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i32(i32 15, i32 3, i32 3,{{.*}}, i32 0, i32 0), !invariant.load
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.struct.buffer.load.format.v4f32({{.*}}, i32 5, i32 0, i32 0, i32 0), !invariant.load
; SHADERTEST: call i32 @llvm.amdgcn.image.load.2d.i32.i32(i32 1, i32 6, i32 6, <8 x i32> %{{[-0-9A-Za0z_.]+}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32(i32 15, i32 6, i32 6,{{.*}},{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
