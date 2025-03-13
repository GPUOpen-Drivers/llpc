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


layout(set = 0, binding = 0) uniform sampler2D samp2D[4];

layout(set = 1, binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 fragColor;

vec4 func(sampler2D s2D, vec2 coord)
{
    return texture(s2D, coord);
}

void main()
{
    vec4 color = func(samp2D[i], vec2(0.5));
    color += func(samp2D[1], vec2(1.0));

    fragColor = color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: call {{.*}} <4 x float> @{{.*}}
; SHADERTEST: define internal {{.*}} <4 x float> @{{.*}}({ ptr addrspace(4), i32, i32, ptr addrspace(4), i32, i32 } %{{[a-zA-Z0-9]+}}, ptr addrspace(5) %{{[a-z0-9]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
