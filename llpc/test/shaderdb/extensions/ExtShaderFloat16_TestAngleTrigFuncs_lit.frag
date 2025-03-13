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
    vec3 fv3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    f16vec3 f16v3_1 = f16vec3(fv3);
    f16vec3 f16v3_2 = f16vec3(fv3);

    f16v3_1 = radians(f16v3_1);
    f16v3_1 = degrees(f16v3_1);
    f16v3_1 = sin(f16v3_1);
    f16v3_1 = cos(f16v3_1);
    f16v3_1 = tan(f16v3_1);
    f16v3_1 = asin(f16v3_1);
    f16v3_1 = acos(f16v3_1);
    f16v3_1 = atan(f16v3_1, f16v3_2);
    f16v3_1 = atan(f16v3_1);
    f16v3_1 = sinh(f16v3_1);
    f16v3_1 = cosh(f16v3_1);
    f16v3_1 = tanh(f16v3_1);
    f16v3_1 = asinh(f16v3_1);
    f16v3_1 = acosh(f16v3_1);
    f16v3_1 = atanh(f16v3_1);

    fragColor = f16v3_1;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <3 x half> %{{.*}}, {{(splat \(half 0xH2478\))|(<half 0xH2478, half 0xH2478, half 0xH2478>)}}
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <3 x half> %{{.*}}, {{(splat \(half 0xH5329\))|(<half 0xH5329, half 0xH5329, half 0xH5329>)}}
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.sin.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.cos.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.tan.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.asin.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.acos.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.atan2.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.sinh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.cosh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.tanh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.asinh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.acosh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.atanh.v3f16(<3 x half>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
