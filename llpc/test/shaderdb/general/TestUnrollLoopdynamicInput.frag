/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
// The loop that involves operations on input variables is expected to be unrolled.

// RUN: amdllpc -stop-after=lgc-add-loop-metadata -o - %s | FileCheck -check-prefixes=SHADERTEST %s

#version 450

layout(location = 0) in vec4 intp[30];
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 s = vec4(0.0);

    for (int i = 0; i < 30; i++)
    {
        const int j = (i & 0xf) - 8;
        const vec2 offset = ivec2(i,j) / float(16.0);
	    s += interpolateAtOffset(intp[i], offset);
    }

    frag_color.x = s.x;
    frag_color.y = s.y;
    frag_color.z = float(0.0);
    frag_color.w = float(1.0);
}

// BEGIN_SHADERTEST
// SHADERTEST: [[LOOP9:![0-9]+]] = distinct !{[[LOOP9]], [[META10:![0-9]+]], [[META11:![0-9]+]]}
// SHADERTEST: [[META10]] = !{!"llvm.loop.unroll.count", i32 31}
// SHADERTEST: [[META11]] = !{!"llvm.loop.disable_nonforced"}
// END_SHADERTEST
