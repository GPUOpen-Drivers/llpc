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
    dmat2 dm2_0;
    dmat2 dm2_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);


    dmat2 dm2_2 = dm2_0 * dm2_1;

    fragColor = (dm2_2[0] == dm2_0[1]) ? vec4(1.0) : color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [2 x <2 x double>] (...) @lgc.create.matrix.times.matrix.a2v2f64([2 x <2 x double>] %{{[^, ]*}}, [2 x <2 x double>] %{{[^, ]*}})

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: fmul {{.*}}double %{{[^, ]*}}, %{{[^, ]*}}
; SHADERTEST: fadd {{.*}}double %{{[^, ]*}}, %{{[^, ]*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
