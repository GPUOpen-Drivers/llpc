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

layout(std140, binding = 0) uniform Uniforms
{
    int64_t  i64;
    u64vec3  u64v3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    u64vec3 u64v3_0 = u64vec3(i64);
    u64v3_0 |= u64v3;
    u64v3_0 &= u64v3;
    u64v3_0 ^= u64v3;
    u64v3_0 = ~u64v3_0;

    fragColor = vec3(u64v3_0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: or <3 x i64> %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST: and <3 x i64> %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST: xor <3 x i64> %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST: xor <3 x i64> %{{[0-9]*}}, {{(splat \(i64 -1\))|(<i64 -1, i64 -1, i64 -1>)}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
