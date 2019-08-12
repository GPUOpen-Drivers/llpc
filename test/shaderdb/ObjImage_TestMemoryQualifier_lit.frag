#version 450

layout(set = 0, binding = 0, rgba32f) coherent readonly uniform image1D img1D;
layout(set = 0, binding = 1, rgba32f) restrict uniform image2D img2D;
layout(set = 0, binding = 2, rgba32f) volatile writeonly uniform image2DRect img2DRect;

void main()
{
    vec4 texel = vec4(0.0);
    texel += imageLoad(img1D, 1);
    texel += imageLoad(img2D, ivec2(2));

    imageStore(img2DRect, ivec2(3), texel);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call <4 x float> (...) @llpc.call.image.load.v4f32(i32 0, i32 0, <8 x i32>
; SHADERTEST: call <4 x float> (...) @llpc.call.image.load.v4f32(i32 1, i32 0, <8 x i32>
; SHADERTEST: call void (...) @llpc.call.image.store(i32 1, i32 0, <8 x i32>
; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call <4 x float> (...) @llpc.call.image.load.v4f32(i32 0, i32 0, <8 x i32>
; SHADERTEST: call <4 x float> (...) @llpc.call.image.load.v4f32(i32 1, i32 0, <8 x i32>
; SHADERTEST: call void (...) @llpc.call.image.store(i32 1, i32 0, <8 x i32>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
