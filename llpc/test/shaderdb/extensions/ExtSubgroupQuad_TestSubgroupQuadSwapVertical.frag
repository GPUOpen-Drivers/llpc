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

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} FE lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.subgroup.quad.swap.vertical.v4f32
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450

#extension GL_KHR_shader_subgroup_quad : require

layout(location = 0) out vec4 result;

layout(set = 0, binding = 4, std430) readonly buffer Buffer0
{
  vec4 data[];
};

void main (void)
{
  result = subgroupQuadSwapVertical(data[gl_SubgroupInvocationID]);
}
