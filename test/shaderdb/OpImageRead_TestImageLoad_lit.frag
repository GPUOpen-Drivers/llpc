#version 450

layout(set = 0, binding = 0, rgba32f) uniform image1D           img1D;
layout(set = 0, binding = 1, rgba32f) uniform image2DRect       img2DRect;
layout(set = 1, binding = 0, rgba32f) uniform imageBuffer       imgBuffer[4];
layout(set = 2, binding = 0, rgba32f) uniform imageCubeArray    imgCubeArray[4];
layout(set = 0, binding = 2, rgba32f) uniform image2DMS         img2DMS;

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = vec4(0.0);
    f4 += imageLoad(img1D, 1);
    f4 += imageLoad(img2DRect, ivec2(2, 3));
    f4 += imageLoad(imgBuffer[index], 4);
    f4 += imageLoad(imgCubeArray[index + 1], ivec3(5, 6, 7));
    f4 += imageLoad(img2DMS, ivec2(8, 9), 2);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.read.f32.1D{{.*}}({{.*}}, i32 1, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.read.f32.Rect{{.*}}({{.*}}, <2 x i32> <i32 2, i32 3>, i32 0,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.texel.buffer.desc.v4i32(i32 1, i32 0, i32 %{{[0-9]+}}, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.read.f32.Buffer({{.*}}, i32 4, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 2, i32 0,{{.*}}, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.read.f32.CubeArray{{.*}}({{.*}}, <3 x i32> <i32 5, i32 6, i32 7>, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 2, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.read.f32.2D.sample{{.*}}({{.*}}, <2 x i32> <i32 8, i32 9>, i32 2, i32 0,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.1d.v4f32.i32(i32 15, i32 1,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i32(i32 15, i32 2, i32 3,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.struct.buffer.load.format.v4f32({{.*}}, i32 4, i32 0, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.cube.v4f32.i32(i32 15, i32 5, i32 6, i32 7,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32(i32 15, i32 8, i32 9, i32 2,{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
