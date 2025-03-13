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


#extension GL_ARB_gpu_shader_int64 : enable

layout(std430, binding = 0) buffer Buffers
{
    ivec3    i3;
    uvec3    u3;
    vec3     f3;
    dvec3    d3;
    i64vec3  i64v3;
    u64vec3  u64v3;
};

void main()
{
    i64v3  = i64vec3(f3);
    i64v3 += i64vec3(d3);

    u64v3  = u64vec3(f3);
    u64v3 += u64vec3(d3);

    f3  = vec3(i64v3);
    f3 += vec3(u64v3);

    d3  = dvec3(i64v3);
    d3 += dvec3(u64v3);

    i3 = ivec3(i64v3);
    i64v3 += i64vec3(i3);

    u3 = uvec3(u64v3);
    u64v3 += u64vec3(u3);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: fptosi <3 x float> %{{[0-9]*}} to <3 x i64>
; SHADERTEST: fptosi <3 x double> %{{[0-9]*}} to <3 x i64>
; SHADERTEST: fptoui <3 x float> %{{[0-9]*}} to <3 x i64>
; SHADERTEST: fptoui <3 x double> %{{[0-9]*}} to <3 x i64>
; SHADERTEST: sitofp <3 x i64> %{{[0-9]*}} to <3 x float>
; SHADERTEST: uitofp <3 x i64> %{{[0-9]*}} to <3 x float>
; SHADERTEST: sitofp <3 x i64> %{{[0-9]*}} to <3 x double>
; SHADERTEST: uitofp <3 x i64> %{{[0-9]*}} to <3 x double>
; SHADERTEST: trunc <3 x i64> %{{[0-9]*}} to <3 x i32>
; SHADERTEST: sext <3 x i32> %{{[0-9]*}} to <3 x i64>
; SHADERTEST: zext <3 x i32> %{{[0-9]*}} to <3 x i64>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
