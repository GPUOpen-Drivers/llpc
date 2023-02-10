// This test checks if load access to a matrix and its underlying vector using a runtime index
// will be transformed to an out of bounds checks against the matrix and vector bounds, moving the loads into conditionally executed BBs.
// The OOB check will possibly zero out the load results.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s -enable-scratch-bounds-checks | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: .[[entry:[a-z0-9]+]]:
; SHADERTEST: %[[mat:[a-z0-9]+]] = alloca [4 x <4 x float>], align 16, addrspace(5)
; SHADERTEST: load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[idx1:[0-9]+]] = add i32 %{{.*}}, 1
; SHADERTEST: load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[idx2:[0-9]+]] = add i32 %{{.*}}, 2
; SHADERTEST: %[[gep:[0-9]+]] = getelementptr [4 x <4 x float>], ptr addrspace(5) %[[mat]], i32 0, i32 %[[idx1]], i32 %[[idx2]]
; SHADERTEST-NEXT: %[[cmp1:[0-9]+]] = icmp ult i32 %[[idx1]], 4
; SHADERTEST-NEXT: %[[cmp2:[0-9]+]] = icmp ult i32 %[[idx2]], 4
; SHADERTEST-NEXT: %[[finalcmp:[0-9]+]] = and i1 %[[cmp1]], %[[cmp2]]
; SHADERTEST-NEXT: br i1 %[[finalcmp]], label %{{.*}}, label %{{.*}}
; SHADERTEST: [[load:[a-z0-9]+]]:
; SHADERTEST: %[[loadResult:[0-9]+]] = load float, ptr addrspace(5) %[[gep]], align 4
; SHADERTEST: [[final:[a-z0-9]+]]:
; SHADERTEST: %{{.*}} = phi float [ 0.000000e+00, %.[[entry]] ], [ %[[loadResult]], %[[load]] ]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450 core

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFragColor;
layout (binding = 0) uniform Constant { int array_index; } c;

void main() {
  mat4 data;
  outFragColor = inColor * data[c.array_index + 1][c.array_index + 2];
}
