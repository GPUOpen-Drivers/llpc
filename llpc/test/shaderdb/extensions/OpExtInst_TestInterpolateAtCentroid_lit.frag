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


layout(location = 0) in centroid float f1_1;
layout(location = 1) in vec4 f4_1;

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = interpolateAtCentroid(f1_1);

    vec4 f4_0 = interpolateAtCentroid(f4_1);

    fragColor = (f4_0.y == f1_0) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @interpolateAtCentroid.f32.p64(ptr addrspace(64) @{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @interpolateAtCentroid.v4f32.p64(ptr addrspace(64) @{{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: %{{[A-Za-z0-9]*}} = call reassoc nnan nsz arcp contract afn <2 x float> @lgc.input.import.builtin.InterpPerspCentroid.v2f32.i32(i32 268435458)
; SHADERTEST: %{{[A-Za-z0-9]*}} = call reassoc nnan nsz arcp contract afn <2 x float> @lgc.input.import.builtin.InterpPerspCentroid.v2f32.i32(i32 268435458)
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p1(float %{{.*}}, i32 0, i32 0, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p2(float %{{.*}}, float %{{.*}}, i32 0, i32 0, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p1(float %{{.*}}, i32 1, i32 1, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p2(float %{{.*}}, float %{{.*}}, i32 1, i32 1, i32 %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
