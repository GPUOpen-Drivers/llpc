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

// This test checks if load access on an array where the index is another member of the array which is fetched by using a runtime index
// will be transformed to multiple out of bounds checks against the bounds of the array, moving the loads into  conditionally executed BBs.
// The OOB check will possibly zero out the load results.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: .[[entry:[a-z0-9]+]]:
; SHADERTEST: %[[arr:[a-z0-9]+]] = alloca [5 x i32], align 4, addrspace(5)
; SHADERTEST: %[[idx1:[0-9]+]] = load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[gep1:[0-9]+]] = getelementptr [5 x i32], ptr addrspace(5) %[[arr]], i32 0, i32 %[[idx1]]
; SHADERTEST: %[[cmp1:[0-9]+]] = icmp ult i32 %[[idx1]], 5
; SHADERTEST: br i1 %[[cmp1]], label %{{.*}}, label %{{.*}}
; SHADERTEST: [[load1:[a-z0-9]+]]:
; SHADERTEST: %[[loadResult1:[0-9]+]] = load i32, ptr addrspace(5) %[[gep1]], align 4
; SHADERTEST: [[final1:[a-z0-9]+]]:
; SHADERTEST: %[[phi:[0-9]+]] = phi i32 [ 0, %.[[entry]] ], [ %[[loadResult1]], %[[load1]] ]
; SHADERTEST: %[[gep2:[0-9]+]] = getelementptr [5 x i32], ptr addrspace(5) %[[arr]], i32 0, i32 %[[phi]]
; SHADERTEST: %[[cmp2:[0-9]+]] = icmp ult i32 %[[phi]], 5
; SHADERTEST: br i1 %[[cmp2]], label %{{.*}}, label %{{.*}}
; SHADERTEST: [[load2:[a-z0-9]+]]:
; SHADERTEST: %[[loadResult2:[0-9]+]] = load i32, ptr addrspace(5) %[[gep2]], align 4
; SHADERTEST: [[final2:[a-z0-9]+]]:
; SHADERTEST: %{{.*}} = phi i32 [ 0, %[[final1]] ], [ %[[loadResult2]], %[[load2]] ]

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450 core

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFragColor;
layout (binding = 0) uniform Constant { int array_index; } c;

void main() {
  int indices[5];
  float el = float(indices[indices[c.array_index]]);
  outFragColor = inColor * vec4(el);
}
