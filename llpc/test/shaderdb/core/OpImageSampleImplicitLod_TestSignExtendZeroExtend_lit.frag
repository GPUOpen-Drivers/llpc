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


layout(rgba32f, binding = 1)    uniform  image2D         i2D;
layout(r32i,    binding = 12)   uniform iimage2D        ii2D;
layout(r32ui,   binding = 12)   uniform uimage2D        ui2D;

layout(rgba32f, binding = 9)    uniform  image2DMS     i2DMS;
layout(r32i,    binding = 13)   uniform iimage2DMS    ii2DMS;
layout(r32ui,   binding = 13)   uniform uimage2DMS    ui2DMS;

layout(location = 0) flat in ivec2 ic2D;
layout(location = 2) flat in uint value;

layout(location = 0) out vec4 fragData;

void main()
{
    vec4 v = vec4(0.0);
    ivec4 iv = ivec4(0.0);
    uvec4 uv = uvec4(0.0);

    v += imageLoad(i2D, ic2D);
    imageStore(i2D, ic2D, v);
    v += imageLoad(ii2D, ic2D);
    imageStore(ii2D, ic2D, iv);
    v += imageLoad(ui2D, ic2D);
    imageStore(ui2D, ic2D, uv);

    v += imageLoad(i2DMS, ic2D, 1);
    imageStore(i2DMS, ic2D, 2, v);
    v += imageLoad(ii2DMS, ic2D, 1);
    imageStore(ii2DMS, ic2D, 2, iv);
    v += imageLoad(ui2DMS, ic2D, 1);
    imageStore(ui2DMS, ic2D, 2, uv);

    fragData = v;
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
