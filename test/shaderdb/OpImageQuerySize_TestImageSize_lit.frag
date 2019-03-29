#version 450

layout(set = 0, binding = 0, rgba32f) uniform image1D           img1D;
layout(set = 0, binding = 1, rgba32f) uniform image2DRect       img2DRect;
layout(set = 0, binding = 2, rgba32f) uniform image2DMS         img2DMS;
layout(set = 1, binding = 0, rgba32f) uniform imageBuffer       imgBuffer[4];
layout(set = 2, binding = 0, rgba32f) uniform imageCubeArray    imgCubeArray[4];

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    ivec3 i3 = ivec3(0);
    i3.x  += imageSize(img1D);
    i3.xy += imageSize(img2DRect);
    i3.xy += imageSize(img2DMS);
    i3.x  += imageSize(imgBuffer[index]);
    i3    += imageSize(imgCubeArray[index + 1]);

    fragColor = (i3.x != 5) ? vec4(1.0) : vec4(0.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.sizelod.1D.i32{{.*}}({{.*}}, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <2 x i32> @llpc.image.querynonlod.sizelod.2D.v2i32{{.*}}({{.*}}, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 2, i32 0, i1 false)
; SHADERTEST: call <2 x i32> @llpc.image.querynonlod.sizelod.2D.sample.v2i32{{.*}}({{.*}}, i32 0,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.texel.buffer.desc.v4i32(i32 1, i32 0, i32 %{{[0-9]+}}, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.sizelod.Buffer.i32({{.*}}, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 2, i32 0,{{.*}}, i1 false)
; SHADERTEST: call <3 x i32> @llpc.image.querynonlod.sizelod.CubeArray.v3i32{{.*}}({{.*}}, i32 0,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call float @llvm.amdgcn.image.getresinfo.1d.f32.i32(i32 1, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: call float @llvm.amdgcn.image.getresinfo.2d.f32.i32(i32 1, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: call float @llvm.amdgcn.image.getresinfo.2dmsaa.f32.i32(i32 1, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: call float @llvm.amdgcn.image.getresinfo.cube.f32.i32(i32 1, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
