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

// Shadow table is disabled, F-mask doesn't need to load descriptor, append the provided sample number to coordinates.

// BEGIN_SHADERTEST
// RUN: amdllpc -v %gfxip --enable-shadow-desc=false %s | FileCheck -check-prefix=SHADERTEST %s
// SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
// SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0
// SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 5, i32 5, i64 0, i32 0
// SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.with.fmask.v4f32(i32 7, i32 1536, {{.*}}, i32 2)

// SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
// "i32 2" is provided sample number
// SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2darraymsaa.v4f32.i32{{(\.v8i32)?}}(i32 15, {{.*}}, {{.*}}, {{.*}}, i32 2, <8 x i32> %{{[0-9]*}}, i32 0, i32 0)
// SHADERTEST: AMDLLPC SUCCESS
// END_SHADERTEST

#version 450 core

layout(set = 0, binding = 0) uniform sampler2DMSArray samp;
layout(location = 0) in vec3 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    ivec3 iUV = ivec3(inUV);
    oColor = texelFetch(samp, iUV, 2);
}
