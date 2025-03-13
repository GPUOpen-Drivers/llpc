#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
#extension GL_AMD_gpu_shader_int16: enable

layout(vertices = 3) out;

layout(location = 0) in f16vec3 f16v3[];
layout(location = 1) in float16_t f16v1[];

layout(location = 2) in i16vec3 i16v3[];
layout(location = 3) in uint16_t u16v1[];

layout(location = 0) out float fv1[];
layout(location = 1) out vec3 fv3[];

void main(void)
{
    fv1[gl_InvocationID] = f16v1[gl_InvocationID];
    fv3[gl_InvocationID] = f16v3[gl_InvocationID];

    fv1[gl_InvocationID] += u16v1[gl_InvocationID];
    fv3[gl_InvocationID] += i16v3[gl_InvocationID];

    gl_TessLevelInner[0] = 1.0;
    gl_TessLevelOuter[0] = 1.0;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call half @lgc.input.import.generic__f16{{.*}}
; SHADERTEST: call <3 x half> @lgc.input.import.generic__v3f16{{.*}}
; SHADERTEST: call i16 @lgc.input.import.generic__i16{{.*}}
; SHADERTEST: call <3 x i16> @lgc.input.import.generic__v3i16{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
