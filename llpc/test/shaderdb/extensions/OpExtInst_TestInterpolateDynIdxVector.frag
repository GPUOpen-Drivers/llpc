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


layout(location = 0) out vec4 frag_color;
layout(location = 0) centroid in vec2 interp;
layout(location = 1) centroid in vec2 interp2[2];
layout(push_constant) uniform PushConstants {
  uint component;
};

void main()
{
    frag_color.x = interpolateAtSample(interp[component], gl_SampleID);
    frag_color.y = interpolateAtSample(interp2[component][0], gl_SampleID);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @interpolateAtSample.f32.p64.i32(ptr addrspace(64) %{{.*}}, i32 %{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.input.import.interpolated__v2f32(i1 false, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.input.import.interpolated__f32(i1 false, i32 1
: SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.input.import.interpolated__f32(i1 false, i32 2
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
