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


layout(binding = 0) uniform Uniforms
{
    double d1_1;
    dvec3 d3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1;
    double d1_0 = frexp(d1_1, i1);

    ivec3 i3;
    dvec3 d3_0 = frexp(d3_1, i3);

    fragColor = ((d3_0.x != d1_0) || (i3.x != i1)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results

; SHADERTEST: = call double (...) @lgc.create.extract.significand.f64(double
; SHADERTEST: = call i32 (...) @lgc.create.extract.exponent.i32(double
; SHADERTEST: = call <3 x double> (...) @lgc.create.extract.significand.v3f64(<3 x double>
; SHADERTEST: = call <3 x i32> (...) @lgc.create.extract.exponent.v3i32(<3 x double>
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: %{{[0-9]*}} = call double @llvm.amdgcn.frexp.mant.f64(double %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call double @llvm.amdgcn.frexp.mant.f64(double %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call i32 @llvm.amdgcn.frexp.exp.i32.f64(double %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call i32 @llvm.amdgcn.frexp.exp.i32.f64(double %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
