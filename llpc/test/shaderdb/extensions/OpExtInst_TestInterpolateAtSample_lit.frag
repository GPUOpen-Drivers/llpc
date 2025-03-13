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
layout(location = 1) in flat sample vec4 f4_1;

layout(binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = interpolateAtSample(f1_1, i);

    vec4 f4_0 = interpolateAtSample(f4_1, i);

    fragColor = (f4_0.y == f1_0) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @interpolateAtSample.f32.p64.i32(ptr addrspace(64) @{{.*}}, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @interpolateAtSample.v4f32.p64.i32(ptr addrspace(64) @{{.*}}, i32 %{{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <2 x float> @lgc.input.import.builtin.SamplePosOffset.v2f32.i32.i32(i32 268435463
; SHADERTEST: call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.eval.Ij.offset.smooth__v2f32(<2 x float>
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.input.import.interpolated__f32(i1 false
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p1(float %{{.*}}, i32 0, i32 0, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p2(float %{{.*}}, float %{{.*}}, i32 0, i32 0, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.mov(i32 {{.*}}2, i32 1, i32 1, i32 %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
