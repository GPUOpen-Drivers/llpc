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

layout(set = 0, binding = 0) uniform sampler2D          samp2D[4];
layout(set = 1, binding = 0) uniform sampler3D          samp3D;
layout(set = 2, binding = 0) uniform samplerCube        sampCube;
layout(set = 3, binding = 0) uniform sampler2DArray     samp2DArray;
layout(set = 4, binding = 0) uniform samplerCubeArray   sampCubeArray;

layout(set = 5, binding = 0) uniform Uniforms
{
    int   index;
    float lodClamp;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0);
    vec4 texel = vec4(0.0);

    sparseTextureGradClampARB(samp2D[index], vec2(0.1), vec2(0.2), vec2(0.3), lodClamp, texel);
    fragColor += texel;

    sparseTextureGradClampARB(samp3D, vec3(0.1), vec3(0.2), vec3(0.3), lodClamp, texel);
    fragColor += texel;

    sparseTextureGradClampARB(sampCube, vec3(0.1), vec3(0.2), vec3(0.3), lodClamp, texel);
    fragColor += texel;

    sparseTextureGradClampARB(samp2DArray, vec3(0.1), vec2(0.2), vec2(0.3), lodClamp, texel);
    fragColor += texel;

    sparseTextureGradClampARB(sampCubeArray, vec4(0.1), vec3(0.2), vec3(0.3), lodClamp, texel);
    fragColor += texel;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
