#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/


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
    f4 += fragmentFetchAMD(s2DMS, ivec2(2, 3), fragIndex);

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
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 6, i32 512,
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 2, i32 512,
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 7, i32 512,
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 1, i32 544,
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 6, i32 544,
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call i32 @llvm.amdgcn.image.load.2d.i32.i16{{(\.v8i32)?}}(i32 1, i16 2, i16 3, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32{{(\.v8i32)?}}(i32 15, i32 2, i32 3, i32
; SHADERTEST: call i32 @llvm.amdgcn.image.load.3d.i32.i16{{(\.v8i32)?}}(i32 1, i16 2, i16 3, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.load.2darraymsaa.v4i32.i32{{(\.v8i32)?}}(i32 15, i32 2, i32 3, i32 1, i32
; SHADERTEST: call i32 @llvm.amdgcn.image.load.2d.i32.i32{{(\.v8i32)?}}(i32 1, i32 %{{.*}}, i32 %{{.*}}, <8 x i32>
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.load.2dmsaa.v4i32.i32{{(\.v8i32)?}}(i32 15, i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
