#version 460
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

#extension GL_EXT_samplerless_texture_functions : require

// BEGIN_SHADERTEST

// RUN: amdllpc -verify-ir -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

// SHADERTEST-LABEL: {{^// LLPC}} FE lowering results

// SHADERTEST: @{{.*}} = internal unnamed_addr addrspace(4) constant [16 x <2 x float>] [<2 x float> {{(splat \(float 6\.250000e\-02\))|(<float 6\.250000e\-02, float 6\.250000e\-02>)}}, <2 x float> <float -6.250000e-02, float -1.875000e-01>, <2 x float> <float -1.875000e-01, float 1.250000e-01>, <2 x float> <float 2.500000e-01, float -6.250000e-02>, <2 x float> <float -3.125000e-01, float -1.250000e-01>, <2 x float> <float 1.250000e-01, float 3.125000e-01>, <2 x float> <float 3.125000e-01, float 1.875000e-01>, <2 x float> <float 1.875000e-01, float -3.125000e-01>, <2 x float> <float -1.250000e-01, float 3.750000e-01>, <2 x float> <float 0.000000e+00, float -4.375000e-01>, <2 x float> <float -2.500000e-01, float -3.750000e-01>, <2 x float> <float -3.750000e-01, float 2.500000e-01>, <2 x float> <float -5.000000e-01, float 0.000000e+00>, <2 x float> <float 4.375000e-01, float -2.500000e-01>, <2 x float> <float 3.750000e-01, float 4.375000e-01>, <2 x float> <float -4.375000e-01, float -5.000000e-01>]
// SHADERTEST: @{{.*}} = internal unnamed_addr addrspace(4) constant [8 x <2 x float>] [<2 x float> <float 6.250000e-02, float -1.875000e-01>, <2 x float> <float -6.250000e-02, float 1.875000e-01>, <2 x float> <float 3.125000e-01, float 6.250000e-02>, <2 x float> <float -1.875000e-01, float -3.125000e-01>, <2 x float> <float -3.125000e-01, float 3.125000e-01>, <2 x float> <float -4.375000e-01, float -6.250000e-02>, <2 x float> <float 1.875000e-01, float 4.375000e-01>, <2 x float> <float 4.375000e-01, float -4.375000e-01>]
// SHADERTEST: @{{.*}} = internal unnamed_addr addrspace(4) constant [4 x <2 x float>] [<2 x float> <float -1.250000e-01, float -3.750000e-01>, <2 x float> <float 3.750000e-01, float -1.250000e-01>, <2 x float> <float -3.750000e-01, float 1.250000e-01>, <2 x float> <float 1.250000e-01, float 3.750000e-01>]
// SHADERTEST: @{{.*}} = internal unnamed_addr addrspace(4) constant [2 x <2 x float>] [<2 x float> {{(splat \(float 2\.500000e\-01\))|(<float 2\.500000e\-01, float 2\.500000e\-01>)}}, <2 x float> {{(splat \(float \-2\.500000e\-01\))|(<float \-2\.500000e\-01, float \-2\.500000e\-01>)}}]

// SHADERTEST: AMDLLPC SUCCESS

// END_SHADERTEST


layout(location = 1) in vec4 i0;
layout(location = 0) out vec4 o0;

vec4 getSamplePosition(int count, inout int index)
{
    vec2 _171[2] = vec2[](vec2(0.25), vec2(-0.25));
    vec2 _188[4] = vec2[](vec2(-0.125, -0.375), vec2(0.375, -0.125), vec2(-0.375, 0.125), vec2(0.125, 0.375));
    vec2 _213[8] = vec2[](vec2(0.0625, -0.1875), vec2(-0.0625, 0.1875), vec2(0.3125, 0.0625), vec2(-0.1875, -0.3125), vec2(-0.3125, 0.3125), vec2(-0.4375, -0.0625), vec2(0.1875, 0.4375), vec2(0.4375, -0.4375));
    vec2 _240[16] = vec2[](vec2(0.0625), vec2(-0.0625, -0.1875), vec2(-0.1875, 0.125), vec2(0.25, -0.0625), vec2(-0.3125, -0.125), vec2(0.125, 0.3125), vec2(0.3125, 0.1875), vec2(0.1875, -0.3125), vec2(-0.125, 0.375), vec2(0.0, -0.4375), vec2(-0.25, -0.375), vec2(-0.375, 0.25), vec2(-0.5, 0.0), vec2(0.4375, -0.25), vec2(0.375, 0.4375), vec2(-0.4375, -0.5));
    index = clamp(index, 0, count - 1);
    vec2 pos;
    switch (count)
    {
        case 2:
        {
            pos = _171[index];
            break;
        }
        case 4:
        {
            pos = _188[index];
            break;
        }
        case 8:
        {
            pos = _213[index];
            break;
        }
        case 16:
        {
            pos = _240[index];
            break;
        }
        default:
        {
            pos = vec2(0.0);
            break;
        }
    }
    return vec4(pos, 0.0, 0.0);
}

void main()
{
    int count = int(uint(i0.x));
    int index = int(uint(gl_SampleID));
    vec4 samplePosition = getSamplePosition(count, index);
    o0.xy = samplePosition.xy;
}

