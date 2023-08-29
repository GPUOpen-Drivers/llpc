// This test checks if an OOB check will get optimized away. It does so by checking if there
// are no phi nodes left at the end.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s -enable-scratch-bounds-checks | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} final pipeline module info
; SHADERTEST: define dllexport amdgpu_ps {{.*}} @_amdgpu_ps_main
; SHADERTEST-NOT: phi
; SHADERTEST: ret
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450 core

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFragColor;
layout (binding = 0) uniform Constant { uint array_index; vec4 data[14]; } c;

struct NestedStruct {
  vec4 nested[4];
};

struct BaseStruct {
	vec4 data[10];
	NestedStruct data2[12];
};

void main() {
  BaseStruct checker[5];
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < 10; j++) {
      checker[i].data[j] = c.data[i + j];
    }
  }

  outFragColor = checker[c.array_index % 5].data[c.array_index % 10];
}
