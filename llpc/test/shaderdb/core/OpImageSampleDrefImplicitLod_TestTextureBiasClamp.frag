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

#extension GL_ARB_sparse_texture_clamp : enable

layout(set = 0, binding = 0) uniform sampler1DShadow            samp1DShadow[4];
layout(set = 1, binding = 0) uniform sampler2DShadow            samp2DShadow;
layout(set = 2, binding = 0) uniform samplerCubeShadow          sampCubeShadow;
layout(set = 3, binding = 0) uniform sampler1DArrayShadow       samp1DArrayShadow;
layout(set = 4, binding = 0) uniform sampler2DArrayShadow       samp2DArrayShadow;
layout(set = 5, binding = 0) uniform samplerCubeArrayShadow     sampCubeArrayShadow;

layout(set = 6, binding = 0) uniform Uniforms
{
    int   index;
    float lodClamp;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0, 0.0, 0.0, 1.0);

    fragColor.x += textureClampARB(samp1DShadow[index], vec3(0.1), lodClamp, 2.0);

    fragColor.x += textureClampARB(samp2DShadow, vec3(0.1), lodClamp, 2.0);

    fragColor.x += textureClampARB(sampCubeShadow, vec4(0.1), lodClamp, 2.0);

    fragColor.x += textureClampARB(samp1DArrayShadow, vec3(0.1), lodClamp, 2.0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
