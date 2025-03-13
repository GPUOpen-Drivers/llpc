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

// This test checks if load access to an array of nested structs which use arrays using some runtime indices
// will be transformed to an out of bounds checks against all accessed elements, moving the load into a conditionally executed BB.
// The OOB check will possibly zero out the load results.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: .[[entry:[a-z0-9]+]]:
; SHADERTEST: %[[arr:[a-z0-9]+]] = alloca [5 x { [10 x <4 x float>], [12 x { [4 x <4 x float>] }] }], align 16, addrspace(5)
; SHADERTEST: {{.*}}:
; SHADERTEST: [[parent:[a-z0-9]+]]:
; SHADERTEST: %[[idx1:[0-9]+]] = load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[tmp:[0-9]+]] = load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[idx2:[0-9]+]] = add i32 %[[tmp]], 2
; SHADERTEST: %[[idx3:[0-9]+]] = load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[gep:[0-9]+]] = getelementptr [5 x { [10 x <4 x float>], [12 x { [4 x <4 x float>] }] }], ptr addrspace(5) %[[arr]], i32 0, i32 %[[idx1]], i32 1, i32 %[[idx2]], i32 0, i32 %[[idx3]]
; SHADERTEST: %[[A:[0-9]+]] = icmp ult i32 %[[idx1]], 5
; SHADERTEST: %[[B:[0-9]+]] = icmp ult i32 %[[idx2]], 12
; SHADERTEST: %[[C:[0-9]+]] = and i1 %[[A]], %[[B]]
; SHADERTEST: %[[D:[0-9]+]] = icmp ult i32 %[[idx3]], 4
; SHADERTEST: %[[cmp:[0-9]+]] = and i1 %[[C]], %[[D]]
; SHADERTEST: br i1 %[[cmp]], label %{{.*}}, label %{{.*}}
; SHADERTEST: [[load:[a-z0-9]+]]:
; SHADERTEST: %[[loadResult:[0-9]+]] = load <4 x float>, ptr addrspace(5) %[[gep]], align 16
; SHADERTEST: br label %{{.*}}
; SHADERTEST: [[final:[a-z0-9]+]]:
; SHADERTEST: %{{.*}} = phi <4 x float> [ zeroinitializer, %{{.*}} ], [ %[[loadResult]], %[[load]] ]

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450 core

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFragColor;
layout (binding = 0) uniform Constant { int array_index; } c;

struct NestedStruct {
  vec4 nested[4];
};

struct BaseStruct {
	vec4 data[10];
	NestedStruct data2[12];
};

void main() {
  BaseStruct checker[5];

  outFragColor = inColor * checker[c.array_index].data[c.array_index + 1] + checker[c.array_index].data2[c.array_index + 2].nested[c.array_index];
}
