#version 450
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

layout(set = 0, binding = 0) uniform sampler2DShadow        samp2DShadow[4];
layout(set = 1, binding = 0) uniform sampler2DRectShadow    samp2DRectShadow;

layout(set = 2, binding = 0) uniform Uniforms
{
    int   index;
};

const ivec2 offsets[4] = { ivec2(1), ivec2(2), ivec2(3), ivec2(4) };

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0);
    vec4 texel = vec4(0.0);

    sparseTextureGatherARB(samp2DShadow[index], vec2(0.1), 1.0, texel);
    fragColor += texel;

    sparseTextureGatherARB(samp2DRectShadow, vec2(1.1), 1.1, texel);
    fragColor += texel;

    sparseTextureGatherOffsetARB(samp2DRectShadow, vec2(0.1), 1.5, ivec2(1), texel);
    fragColor += texel;

    sparseTextureGatherOffsetsARB(samp2DRectShadow, vec2(1.2), 1.1, offsets, texel);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
