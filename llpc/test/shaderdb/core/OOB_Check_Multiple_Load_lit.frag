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

// This test checks if load access to a vector using multiple runtime indices
// will be transformed to out of bounds checks against the vector bounds, moving the load into conditionally executed BBs.
// The OOB check will possibly zero out the load results.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: .[[entry:[a-z0-9]+]]:
; SHADERTEST: %[[arr:[a-z0-9]+]] = alloca <4 x float>, align 16, addrspace(5)
; SHADERTEST: load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[idx1:[0-9]+]] = add i32 %{{.*}}, 1
; SHADERTEST: %[[gep1:[0-9]+]] = getelementptr <4 x float>, ptr addrspace(5) %[[arr]], i32 0, i32 %[[idx1]]
; SHADERTEST-NEXT: %[[cmp1:[0-9]+]] = icmp ult i32 %[[idx1]], 4
; SHADERTEST-NEXT: br i1 %[[cmp1]], label %{{.*}}, label %{{.*}}
; SHADERTEST: [[load1:[a-z0-9]+]]:
; SHADERTEST: %[[loadResult1:[0-9]+]] = load float, ptr addrspace(5) %[[gep1]], align 4
; SHADERTEST: [[final1:[a-z0-9]+]]:
; SHADERTEST: %{{.*}} = phi float [ 0.000000e+00, %.[[entry]] ], [ %[[loadResult1]], %[[load1]] ]
; SHADERTEST: store float %{{.*}}, ptr addrspace(5) %{{.*}}, align 4
; SHADERTEST: load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[idx2:[0-9]+]] = add i32 %{{.*}}, 2
; SHADERTEST: %[[gep2:[0-9]+]] = getelementptr <4 x float>, ptr addrspace(5) %[[arr]], i32 0, i32 %[[idx2]]
; SHADERTEST-NEXT: %[[cmp2:[0-9]+]] = icmp ult i32 %[[idx2]], 4
; SHADERTEST-NEXT: br i1 %[[cmp2]], label %{{.*}}, label %{{.*}}
; SHADERTEST: [[load2:[a-z0-9]+]]:
; SHADERTEST: %[[loadResult2:[0-9]+]] = load float, ptr addrspace(5) %[[gep2]], align 4
; SHADERTEST: [[final2:[a-z0-9]+]]:
; SHADERTEST: %{{.*}} = phi float [ 0.000000e+00, %[[final1]] ], [ %[[loadResult2]], %[[load2]] ]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450 core

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFragColor;
layout (binding = 0) uniform Constant { int array_index; } c;

void main() {
  vec4 data;
  float base1 = data[c.array_index + 1];
  float base2 = data[c.array_index + 2];
  outFragColor = inColor * vec4(base1, base2, base2, base2);
}
