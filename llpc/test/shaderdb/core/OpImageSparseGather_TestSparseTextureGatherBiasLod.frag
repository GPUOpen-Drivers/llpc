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


#extension GL_ARB_sparse_texture2: enable
#extension GL_AMD_texture_gather_bias_lod: enable

layout(binding = 0) uniform sampler2D           s2D;
layout(binding = 1) uniform sampler2DArray      s2DArray;
layout(binding = 2) uniform samplerCube         sCube;
layout(binding = 3) uniform samplerCubeArray    sCubeArray;

layout(location = 1) in vec2 c2;
layout(location = 2) in vec3 c3;
layout(location = 3) in vec4 c4;

layout(location = 4) in float lod;
layout(location = 5) in float bias;

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 texel  = vec4(0.0);
    vec4 result = vec4(0.0);

    const ivec2 offsets[4] = { ivec2(0, 0), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1) };

    sparseTextureGatherARB(s2D,        c2, result, 0, bias);
    texel += result;
    sparseTextureGatherARB(s2DArray,   c3, result, 1, bias);
    texel += result;
    sparseTextureGatherARB(sCube,      c3, result, 2, bias);
    texel += result;
    sparseTextureGatherARB(sCubeArray, c4, result, 2, bias);
    texel += result;

    sparseTextureGatherOffsetARB(s2D,      c2, offsets[0], result, 0, bias);
    texel += result;
    sparseTextureGatherOffsetARB(s2DArray, c3, offsets[1], result, 1, bias);
    texel += result;

    sparseTextureGatherOffsetsARB(s2D,      c2, offsets, result, 0, bias);
    texel += result;
    sparseTextureGatherOffsetsARB(s2DArray, c3, offsets, result, 1, bias);
    texel += result;

    sparseTextureGatherLodAMD(s2D,        c2, lod, result);
    texel += result;
    sparseTextureGatherLodAMD(s2DArray,   c3, lod, result, 1);
    texel += result;
    sparseTextureGatherLodAMD(sCube,      c3, lod, result, 2);
    texel += result;
    sparseTextureGatherLodAMD(sCubeArray, c4, lod, result, 2);
    texel += result;

    sparseTextureGatherLodOffsetAMD(s2D,      c2, lod, offsets[0], result);
    texel += result;
    sparseTextureGatherLodOffsetAMD(s2DArray, c3, lod, offsets[1], result, 1);
    texel += result;

    sparseTextureGatherLodOffsetsAMD(s2D,      c2, lod, offsets, result);
    texel += result;
    sparseTextureGatherLodOffsetsAMD(s2DArray, c3, lod, offsets, result, 1);
    texel += result;

    fragColor = texel;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
