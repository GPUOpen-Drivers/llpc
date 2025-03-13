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


layout(location = 0) in vec3 l0 ;
layout(location = 1) in vec3 l1 ;
layout(location = 2) in vec3 l2 ;
layout(location = 3) in vec3 l3 ;


layout(location = 0) out vec4 color;

void main()
{
    mat3 x = mat3(l0, l1, l2);
    mat3 y = mat3(l1, l2, l3);
    mat3 z = x * y;
    color.xyz = z[0] + z[1] + z[2];
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [3 x <3 x float>] (...) @lgc.create.matrix.times.matrix.a3v3f32

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: fmul {{.*}}float %{{[^, ]*}}, %{{[^, ]*}}
; SHADERTEST: fadd {{.*}}float %{{[^, ]*}}, %{{[^, ]*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
