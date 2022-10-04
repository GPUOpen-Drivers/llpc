// This test checks if store access to an array inside a array of structs using some runtime indices
// will be transformed to an out of bounds checks against all accessed elements, moving the store into a conditionally executed BB.
// The OOB check will possibly omit the store.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s -enable-scratch-bounds-checks | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: .[[entry:[a-z0-9]+]]:
; SHADERTEST: %[[arr:[a-z0-9]+]] = alloca [5 x { [10 x <4 x float>] }], align 16, addrspace(5)
; SHADERTEST: %[[idx1:[0-9]+]] = load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[tmp:[0-9]+]] = load i32, ptr addrspace(7) @c, align 4
; SHADERTEST: %[[idx2:[0-9]+]] = add i32 %[[tmp]], 1
; SHADERTEST: %[[gep:[0-9]+]] = getelementptr [5 x { [10 x <4 x float>] }], ptr addrspace(5) %[[arr]], i32 0, i32 %[[idx1]], i32 0, i32 %[[idx2]]
; SHADERTEST: icmp ult i32 %{{.*}}, 5
; SHADERTEST: icmp ult i32 %{{.*}}, 10
; SHADERTEST: %[[cmp:[0-9]+]] = and i1 %{{.*}}, %{{.*}}
; SHADERTEST-NEXT: br i1 %{{.*}}, label %{{.*}}, label %{{.*}}
; SHADERTEST: [[store:[a-z0-9]+]]:
; SHADERTEST: store <4 x float> %{{.*}}, ptr addrspace(5) %[[gep]], align 16
; SHADERTEST: br label %{{.*}}
; SHADERTEST: [[final:[a-z0-9]+]]:

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450 core

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFragColor;
layout (binding = 0) uniform Constant { int array_index; } c;

struct BaseStruct {
	vec4 data[10];
};

void main() {
  BaseStruct checker[5];
  checker[c.array_index].data[c.array_index + 1] = inColor * 4.0;

  outFragColor = inColor + checker[c.array_index].data[c.array_index + 1];
}
