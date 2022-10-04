// This test checks if load access on dependent array elements (e. g. using the result of a load as input for another load)  using a runtime index
// will be transformed to multiple out of bounds checks against the bounds of the array, moving the loads into  conditionally executed BBs.
// The OOB check will possibly zero out the load results.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s -enable-scratch-bounds-checks | FileCheck -check-prefix=SHADERTEST %s
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
; SHADERTEST: %{{.*}} = phi i32 [ 0, %.[[entry]] ], [ %[[loadResult1]], %[[load1]] ]
; SHADERTEST: %[[idx2:[0-9]+]] = load i32, ptr addrspace(5) %{{.*}}, align 4
; SHADERTEST: %[[gep2:[0-9]+]] = getelementptr [5 x i32], ptr addrspace(5) %[[arr]], i32 0, i32 %[[idx2]]
; SHADERTEST: %[[cmp2:[0-9]+]] = icmp ult i32 %[[idx2]], 5
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
  vec4 data[2048][2];
  int indices[5];
  int index = indices[c.array_index];
  float el = float(indices[index]);
  outFragColor = inColor * vec4(el);
}
