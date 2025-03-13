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


struct str
{
    float f;
};

layout(set = 0, binding = 0, std430) buffer BB
{
   layout(offset = 128) vec4 m1;
   layout(offset = 256) str m2;
   layout(offset = 512) vec2 m3;
};

layout(set = 1, binding = 0) uniform Uniforms
{
    vec4 u1;
    float u2;
    vec2 u3;
};

layout(location = 0) out vec4 o1;
layout(location = 1) out float o2;
layout(location = 2) out vec2 o3;

void main()
{
    m1 = u1;
    m2.f = u2;
    m3 = u3;

    o1 = m1;
    o2 = m2.f;
    o3 = m3;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC.*}} LGC lowering results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32{{(\.v4i32)?}}(<4 x i32> %{{.*}}, <4 x i32> %{{[0-9]*}}, i32 128
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 256
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2i32{{(\.v4i32)?}}(<2 x i32> %{{.*}}, <4 x i32> %{{[0-9]*}}, i32 512

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
