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


layout(constant_id = 1) const int SIZE = 6;

layout(set = 0, binding = 0) uniform UBO
{
    vec4 fa[SIZE];
};

layout(location = 0) in vec4 input0;

void main()
{
    gl_Position = input0 + fa[gl_VertexIndex % fa.length()];
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call i32 (...) @lgc.create.smod.i32(i32
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: srem i32 %{{.*}}, 6
; SHADERTEST-DAG: icmp slt i32 %{{[0-9]*}}, 0
; SHADERTEST-DAG: icmp ne i32 %{{[0-9]*}}, 0
; SHADERTEST-DAG: and i1 %{{[0-9]*}}, %{{[0-9]*}}
; SHADERTEST-DAG: add {{.*}}i32{{.*}} 6
; SHADERTEST: select i1 %{{[0-9]*}}, i32 %{{[0-9]*}}, i32 %{{[0-9]*}}
; SHADERTEST: getelementptr <{ [6 x [4 x float]] }>,
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
