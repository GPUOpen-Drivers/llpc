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

// Test not forcing NURI
// BEGIN_SHADERTEST
// RUN: amdllpc -v %gfxip %s --force-non-uniform-resource-index-stage-mask=0x00000000 | FileCheck -check-prefix=NOTFORCENURITEST %s
// NOTFORCENURITEST-LABEL: {{^// LLPC}} LGC before-lowering results
// When not forcing NURI (Non Uniform Resource Index), there should be a `readfirstlane`.
// NOTFORCENURITEST: %{{[0-9]+}} = call {{i32|float}} @llvm.amdgcn.readfirstlane{{(.[if]+32)?}}({{i32|float}} %{{[0-9]+}})
// NOTFORCENURITEST: AMDLLPC SUCCESS
// END_SHADERTEST

// Test forcing NURI
// BEGIN_SHADERTEST
// RUN: amdllpc -v %gfxip %s --force-non-uniform-resource-index-stage-mask=0xFFFFFFFF | FileCheck -check-prefix=FORCENURITEST %s
// FORCENURITEST-LABEL: {{^// LLPC}} LGC before-lowering results
// When forcing NURI (Non Uniform Resource Index), there should not be a `readfirstlane`.
// FORCENURITEST-NOT: %{{[0-9]+}} = call {{i32|float}} @llvm.amdgcn.readfirstlane{{(.[if]+32)?}}({{i32|float}} %{{[0-9]+}})
// FORCENURITEST: AMDLLPC SUCCESS
// END_SHADERTEST

#version 450

#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) buffer Data
{
    vec4 color;
} data[];

layout(location = 0) out vec4 FragColor;
layout(location = 0) in flat int index;
void main()
{
  FragColor = data[index].color;
}
