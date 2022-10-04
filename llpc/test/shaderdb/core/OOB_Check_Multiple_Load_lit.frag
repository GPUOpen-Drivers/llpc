// This test checks if load access to a vector using multiple runtime indices
// will be transformed to out of bounds checks against the vector bounds, moving the load into conditionally executed BBs.
// The OOB check will possibly zero out the load results.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s -enable-scratch-bounds-checks | FileCheck -check-prefix=SHADERTEST %s
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
