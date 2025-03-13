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


layout(location = 0, component = 1) in vec2 f2;
layout(location = 0, component = 3) in float f1;

layout(location = 0) out vec3 f3;

void main (void)
{
    f3 = vec3(f1, f2);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call <2 x float> (...) @lgc.input.import.interpolated__v2f32{{.*}}
; SHADERTEST: call float (...) @lgc.input.import.interpolated__f32{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call float @llvm.amdgcn.interp.p1(float %{{[^,]*}}, i32 1, i32 0, i32 %PrimMask)
; SHADERTEST: call float @llvm.amdgcn.interp.p2(float %{{[^,]*}}, float %{{[^,]*}}, i32 1, i32 0, i32 %PrimMask)
; SHADERTEST: call float @llvm.amdgcn.interp.p1(float %{{[^,]*}}, i32 3, i32 0, i32 %PrimMask)
; SHADERTEST: call float @llvm.amdgcn.interp.p2(float %{{[^,]*}}, float %{{[^,]*}}, i32 3, i32 0, i32 %PrimMask)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
