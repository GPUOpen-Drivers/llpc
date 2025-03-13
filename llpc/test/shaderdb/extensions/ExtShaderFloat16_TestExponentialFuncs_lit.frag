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


#extension GL_AMD_gpu_shader_half_float: enable

layout(binding = 0) buffer Buffers
{
    vec3  fv3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    f16vec3 f16v3_1 = f16vec3(fv3);
    f16vec3 f16v3_2 = f16vec3(fv3);
    float16_t f16 = f16v3_1.x;

    f16     = pow(f16, 0.0hf);
    f16     = pow(f16, -2.0hf);
    f16     = pow(f16, 3.0hf);
    f16v3_1 = pow(f16v3_1, f16v3_2);
    f16v3_1 = exp(f16v3_1);
    f16v3_1 = log(f16v3_1);
    f16v3_1 = exp2(f16v3_1);
    f16v3_1 = log2(f16v3_1);
    f16v3_1 = sqrt(f16v3_1);
    f16v3_1 = inversesqrt(f16v3_1);

    fragColor = f16v3_1 + vec3(f16);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-3: = call reassoc nnan nsz arcp contract afn half (...) @lgc.create.power.f16(half
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.power.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.exp.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.log.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.exp2.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.log2.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.sqrt.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.inverse.sqrt.v3f16(<3 x half>
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
