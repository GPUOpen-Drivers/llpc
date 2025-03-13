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

// Make sure that there is a single begin index
// Make sure that there is a single waterfall.readfirstlane for the offset

#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 7) uniform sampler2D _11[];

layout(location = 0) out vec4 _3;
layout(location = 3) flat in int _4;
layout(location = 1) flat in vec2 _6;

void main()
{
    int _12 = _4;
    _3 = texture(_11[nonuniformEXT(_12)], _6);
}

// BEGIN_SHADERTEST
//
// RUN: amdllpc -vgpr-limit=64 -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
// SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
// SHADERTEST: call i32 @llvm.amdgcn.waterfall.begin.i32
// SHADERTEST-NOT: call i32 @llvm.amdgcn.waterfall.begin.i32
// SHADERTEST: call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32
// SHADERTEST-NOT: call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32
// SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.waterfall.end.v4f32
// SHADERTEST: AMDLLPC SUCCESS
//
// END_SHADERTEST
