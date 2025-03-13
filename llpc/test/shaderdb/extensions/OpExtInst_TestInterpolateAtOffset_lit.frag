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
    vec2 offset;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = interpolateAtOffset(f1_1, offset);

    vec4 f4_0 = interpolateAtOffset(f4_1, offset);

    fragColor = (f4_0.y == f1_0) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v -gfxip=11.0.0 %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @interpolateAtOffset.f32.p64.v2f32(ptr addrspace(64) @{{.*}}, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) @{{.*}}, <2 x float> %{{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.eval.Ij.offset.smooth__v2f32(<2 x float>
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.input.import.interpolated__f32(i1 false, i32 0, i32 0, i32 0, i32 poison, i32 0, <2 x float> 
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false
; SHADERTEST-LABEL: _amdgpu_ps_main
; SHADERTEST-COUNT-6: v_fmac_f32_e32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
