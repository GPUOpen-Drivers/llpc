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


#extension GL_EXT_shader_explicit_arithmetic_types_int8: enable

layout(location = 0) in int8_t i8In;
layout(location = 1) in u8vec3 u8v3In;

layout(location = 0) out int8_t i8Out;
layout(location = 1) out u8vec3 u8v3Out;

void main (void)
{
    i8Out   = i8In;
    u8v3Out = u8v3In;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.generic.i32.i32.i8(i32 0, i32 0, i8 %{{[0-9]*}})
; SHADERTEST: call void @lgc.output.export.generic.i32.i32.v3i8(i32 1, i32 0, <3 x i8> %{{[0-9]*}})
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call void @llvm.amdgcn.exp.f32(i32 {{.*}}32, i32 {{.*}}1, float %{{[0-9]*}}, float {{(undef|poison)}}, float {{(undef|poison)}}, float {{(undef|poison)}}, i1 {{.*}}false, i1 {{.*}}false)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
