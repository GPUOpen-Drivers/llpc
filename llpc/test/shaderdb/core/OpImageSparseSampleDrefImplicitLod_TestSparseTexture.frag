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
layout(set = 1, binding = 0) uniform samplerCubeArrayShadow sampCubeArrayShadow;
layout(set = 2, binding = 0) uniform sampler2DRectShadow    samp2DRectShadow;

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0);
    float texel = 0.0;

    sparseTextureARB(samp2DShadow[0], vec3(0.1), texel, 1.0);
    fragColor.x += texel;

    sparseTextureARB(sampCubeArrayShadow, vec4(0.3), 2.0, texel);
    fragColor.y += texel;

    sparseTextureOffsetARB(samp2DShadow[index], vec3(0.2), ivec2(1), texel, 1.0);
    fragColor.z += texel;

    sparseTextureOffsetARB(samp2DRectShadow, vec3(0.2), ivec2(1), texel);
    fragColor.w += texel;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
