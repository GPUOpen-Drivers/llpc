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


#extension GL_AMD_gpu_shader_half_float: enable
#extension GL_AMD_gpu_shader_half_float_fetch: enable
#extension GL_AMD_texture_gather_bias_lod: enable

layout(set = 0, binding =  0) uniform f16sampler1D            s1D;
layout(set = 0, binding =  1) uniform f16sampler2D            s2D;
layout(set = 0, binding =  2) uniform f16sampler3D            s3D;
layout(set = 0, binding =  3) uniform f16sampler2DRect        s2DRect;
layout(set = 0, binding =  4) uniform f16samplerCube          sCube;
layout(set = 0, binding =  5) uniform f16samplerBuffer        sBuffer;
layout(set = 0, binding =  6) uniform f16sampler2DMS          s2DMS;
layout(set = 0, binding =  7) uniform f16sampler1DArray       s1DArray;
layout(set = 0, binding =  8) uniform f16sampler2DArray       s2DArray;
layout(set = 0, binding =  9) uniform f16samplerCubeArray     sCubeArray;
layout(set = 0, binding = 10) uniform f16sampler2DMSArray     s2DMSArray;

layout(set = 0, binding = 11) uniform f16sampler1DShadow          s1DShadow;
layout(set = 0, binding = 12) uniform f16sampler2DShadow          s2DShadow;
layout(set = 0, binding = 13) uniform f16sampler2DRectShadow      s2DRectShadow;
layout(set = 0, binding = 14) uniform f16samplerCubeShadow        sCubeShadow;
layout(set = 0, binding = 15) uniform f16sampler1DArrayShadow     s1DArrayShadow;
layout(set = 0, binding = 16) uniform f16sampler2DArrayShadow     s2DArrayShadow;
layout(set = 0, binding = 17) uniform f16samplerCubeArrayShadow   sCubeArrayShadow;

layout(location =  0) in float c1;
layout(location =  1) in vec2  c2;
layout(location =  2) in vec3  c3;
layout(location =  3) in vec4  c4;

layout(location =  4) in float compare;

layout(location = 0) out vec4 fragColor;

void main()
{
    f16vec4 texel = f16vec4(0.0hf);

    texel   += texture(s1D, c1);
    texel   += texture(s2D, c2);
    texel   += texture(s3D, c3);
    texel   += texture(sCube, c3);
    texel.x += texture(s1DShadow, c3);
    texel.x += texture(s2DShadow, c3);
    texel.x += texture(sCubeShadow, c4);
    texel   += texture(s1DArray, c2);
    texel   += texture(s2DArray, c3);
    texel   += texture(sCubeArray, c4);
    texel.x += texture(s1DArrayShadow, c3);
    texel.x += texture(s2DArrayShadow, c4);
    texel   += texture(s2DRect, c2);
    texel.x += texture(s2DRectShadow, c3);
    texel.x += texture(sCubeArrayShadow, c4, compare);

    fragColor = texel;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
