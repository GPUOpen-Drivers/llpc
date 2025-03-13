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


layout(location = 0) in vec2 vsOut0;
layout(location = 1) in vec3 vsOut1;
layout(location = 2) in vec4 vsOut2;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0)  uniform sampler1D samp1D;
layout(set = 0, binding = 1)  uniform sampler2D samp2D;
layout(set = 0, binding = 2)  uniform sampler3D samp3D;
layout(set = 0, binding = 3)  uniform sampler2DRect samp2DR;
layout(set = 0, binding = 4)  uniform sampler1DShadow samp1DS;
layout(set = 0, binding = 5)  uniform sampler2DShadow samp2DS;
layout(set = 0, binding = 7)  uniform sampler2DRectShadow samp2DRS;

void main()
{
    oColor  = textureProj(samp1D, vsOut0);
    oColor += textureProj(samp2D, vsOut1);
    oColor += textureProj(samp3D, vsOut2);
    oColor += textureProj(samp2DR, vsOut1);
    oColor += vec4(textureProj(samp1DS, vsOut2), 1, 0, 1);
    oColor += vec4(textureProj(samp2DS, vsOut2), 1, 0, 1);
    oColor += vec4(textureProj(samp2DRS, vsOut2), 1, 0, 1);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
