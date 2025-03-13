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


#extension GL_AMD_gpu_shader_int16: enable
#extension GL_AMD_shader_trinary_minmax: enable

layout(binding = 0) buffer Buffers
{
    ivec3  iv3[3];
    uvec3  uv3[3];
};

layout(location = 0) out vec3 fragColor;

void main()
{
    i16vec3 i16v3_1 = i16vec3(iv3[0]);
    i16vec3 i16v3_2 = i16vec3(iv3[1]);
    i16vec3 i16v3_3 = i16vec3(iv3[2]);

    u16vec3 u16v3_1 = u16vec3(uv3[0]);
    u16vec3 u16v3_2 = u16vec3(uv3[1]);
    u16vec3 u16v3_3 = u16vec3(uv3[2]);

    i16vec3 i16v3 = i16vec3(0s);
    i16v3 += min3(i16v3_1, i16v3_2, i16v3_3);
    i16v3 += max3(i16v3_1, i16v3_2, i16v3_3);
    i16v3 += mid3(i16v3_1, i16v3_2, i16v3_3);

    u16vec3 u16v3 = u16vec3(0us);
    u16v3 += min3(u16v3_1, u16v3_2, u16v3_3);
    u16v3 += max3(u16v3_1, u16v3_2, u16v3_3);
    u16v3 += mid3(u16v3_1, u16v3_2, u16v3_3);

    fragColor  = i16v3;
    fragColor += u16v3;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
