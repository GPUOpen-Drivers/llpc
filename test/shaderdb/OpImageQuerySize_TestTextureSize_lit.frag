#version 450

layout(set = 0, binding = 0) uniform sampler2DRect       samp2DRect;
layout(set = 1, binding = 0) uniform samplerBuffer       samp2DBuffer[4];
layout(set = 0, binding = 1) uniform sampler2DMS         samp2DMS;
layout(set = 2, binding = 0) uniform sampler2DMSArray    samp2DMSArray[4];

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out ivec3 i3;

void main()
{
    i3.xy  = textureSize(samp2DRect);
    i3.x  += textureSize(samp2DBuffer[index]);
    i3.xy += textureSize(samp2DMS);
    i3    += textureSize(samp2DMSArray[index]);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <2 x i32> @llpc.image.querynonlod.sizelod.2D.v2i32{{.*}}({{.*}}, i32 0,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.desc.load.texel.buffer.v4i32(i32 1, i32 0, i32 %{{[0-9]+}}, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.sizelod.Buffer.i32({{.*}}, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <2 x i32> @llpc.image.querynonlod.sizelod.2D.sample.v2i32{{.*}}({{.*}}, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 2, i32 0,{{.*}}, i1 false)
; SHADERTEST: call <3 x i32> @llpc.image.querynonlod.sizelod.2DArray.sample.v3i32{{.*}}({{.*}}, i32 0,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getresinfo.2d.v2f32.i32(i32 3, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getresinfo.2dmsaa.v2f32.i32(i32 3, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <3 x float> @llvm.amdgcn.image.getresinfo.2darraymsaa.v3f32.i32(i32 7, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
