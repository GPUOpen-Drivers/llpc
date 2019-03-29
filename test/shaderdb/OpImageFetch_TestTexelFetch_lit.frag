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
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.fetch.f32.1D.lod{{.*}}({{.*}}, i32 2, i32 2,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 1, i32 0,{{.*}}, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.fetch.f32.2D.lod{{.*}}({{.*}}, <2 x i32> <i32 7, i32 7>, i32 8,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.fetch.f32.Rect{{.*}}({{.*}}, <2 x i32> <i32 3, i32 3>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.texel.buffer.desc.v4i32(i32 0, i32 2, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.fetch.f32.Buffer({{.*}}, i32 5,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 3,{{.*}}, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.fetch.f32.2D.sample.fmaskbased{{.*}}({{.*}},{{.*}}, <2 x i32> <i32 6, i32 6>, i32 4,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.mip.1d.v4f32.i32(i32 15, i32 2, i32 2,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.mip.2d.v4f32.i32(i32 15, i32 7, i32 7, i32 8,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i32(i32 15, i32 3, i32 3,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.struct.buffer.load.format.v4f32({{.*}}, i32 5, i32 0, i32 0, i32 0)
; SHADERTEST: call float @llvm.amdgcn.image.load.2d.f32.i32(i32 1, i32 6, i32 6,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32(i32 15, i32 6, i32 6,{{.*}},{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
