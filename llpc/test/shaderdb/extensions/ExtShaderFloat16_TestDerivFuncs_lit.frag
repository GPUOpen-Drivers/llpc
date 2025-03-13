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

layout(binding = 0, std430) buffer Buffers
{
    vec3 fv3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    f16vec3 f16v3 = f16vec3(fv3);

    f16v3 = dFdx(f16v3);
    f16v3 = dFdy(f16v3);
    f16v3 = dFdxFine(f16v3);
    f16v3 = dFdyFine(f16v3);
    f16v3 = dFdxCoarse(f16v3);
    f16v3 = dFdyCoarse(f16v3);
    f16v3 = fwidth(f16v3);
    f16v3 = fwidthFine(f16v3);
    f16v3 = fwidthCoarse(f16v3);

    fragColor = f16v3;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x half>
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
