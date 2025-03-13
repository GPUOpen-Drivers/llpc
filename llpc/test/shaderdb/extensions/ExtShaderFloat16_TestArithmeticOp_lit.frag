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


#extension GL_AMD_gpu_shader_half_float: enable

layout(binding = 0) uniform Uniforms
{
    vec4 fv4;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    f16vec4   f16v4 = f16vec4(fv4);
    float16_t f16   = -f16v4.x;

    f16v4 += f16;
    f16v4 *= f16v4;
    f16v4 -= f16;
    f16v4 /= f16;
    f16v4++;
    f16v4--;

    fragColor = f16v4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: fadd reassoc nnan nsz arcp contract afn <4 x half> %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST: fmul reassoc nnan nsz arcp contract afn <4 x half> %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST: fsub reassoc nnan nsz arcp contract afn <4 x half> %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST: fdiv reassoc nnan nsz arcp contract afn <4 x half> {{(splat \(half 0xH3C00\))|(<half 0xH3C00, half 0xH3C00, half 0xH3C00, half 0xH3C00>)}},
; SHADERTEST: fmul reassoc nnan nsz arcp contract afn <4 x half>
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
