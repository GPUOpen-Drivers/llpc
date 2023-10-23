#version 450 core

#extension GL_AMD_shader_fragment_mask: enable

layout(binding = 0) uniform sampler2DMS       s2DMS;
layout(binding = 1) uniform isampler2DMSArray is2DMSArray;

layout(binding = 2, input_attachment_index = 0) uniform usubpassInputMS usubpassMS;

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = vec4(0.0);

    uint fragMask = fragmentMaskFetchAMD(s2DMS, ivec2(2, 3));
    uint fragIndex = (fragMask & 0xF0) >> 4;
    f4 += fragmentFetchAMD(s2DMS, ivec2(2, 3), 1);

    fragMask = fragmentMaskFetchAMD(is2DMSArray, ivec3(2, 3, 1));
    fragIndex = (fragMask & 0xF0) >> 4;
    f4 += fragmentFetchAMD(is2DMSArray, ivec3(2, 3, 1), fragIndex);

    fragMask = fragmentMaskFetchAMD(usubpassMS);
    fragIndex = (fragMask & 0xF0) >> 4;
    f4 += fragmentFetchAMD(usubpassMS, fragIndex);

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 6, i32 512, <8 x i32>
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 2, i32 512, <8 x i32>
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 7, i32 512, <8 x i32>
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 1, i32 544, <8 x i32>
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 6, i32 544, <8 x i32>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i16
; SHADERTEST: call i32 @llvm.amdgcn.image.load.3d.i32.i16(i32 1, i16 2, i16 3, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.load.2darraymsaa.v4i32.i32(i32 15, i32 2, i32 3, i32 1, i32 %{{[-0-9A-Za0z_.]+}}, <8 x i32> %{{[-0-9A-Za0z_.]+}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.load.2d.i32.i32(i32 1,
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
