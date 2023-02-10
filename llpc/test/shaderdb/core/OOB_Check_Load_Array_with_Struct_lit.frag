// This test checks if load access to an array including a simple struct using a runtime index
// will be transformed to an out of bounds checks against the bounds of the array, moving the load into a conditionally executed BB.
// The OOB check will possibly zero out the load results.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s -enable-scratch-bounds-checks | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: .[[entry:[a-z0-9]+]]:
; SHADERTEST: %[[arr:[a-z0-9]+]] = alloca [5 x { <4 x float> }], align 16, addrspace(5)
; SHADERTEST: %[[idx1:[0-9]+]] = load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[gep:[0-9]+]] = getelementptr [5 x { <4 x float> }], ptr addrspace(5) %[[arr]], i32 0, i32 %[[idx1]], i32 0
; SHADERTEST: %[[cmp:[0-9]+]] = icmp ult i32 %[[A:[0-9]+]], 5
; SHADERTEST-NEXT: br i1 %{{.*}}, label %{{.*}}, label %{{.*}}
; SHADERTEST: [[load:[a-z0-9]+]]:
; SHADERTEST: [[loadResult:[0-9]+]] = load <4 x float>, ptr addrspace(5) %{{.*}}, align 16
; SHADERTEST: [[final:[a-z0-9]+]]:
; SHADERTEST: %{{.*}} = phi <4 x float> [ zeroinitializer, %.[[entry]] ], [ %[[loadResult]], %[[load]] ]

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450 core

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFragColor;
layout (binding = 0) uniform Constant { int array_index; } c;

struct BaseStruct {
	vec4 data;
};

void main() {
  BaseStruct checker[5];

  outFragColor = inColor * checker[c.array_index].data;
}
