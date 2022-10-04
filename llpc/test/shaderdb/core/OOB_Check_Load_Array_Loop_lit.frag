// This test checks if using a loop of unknown length to access an array
// will be transformed to an out of bounds checks against all bounds of the array, moving the load into a conditionally executed BB.
// The OOB check will possibly zero out the load results.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s -enable-scratch-bounds-checks | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s -enable-scratch-bounds-checks | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: .[[parent:[a-z0-9]+]]:
; SHADERTEST: %[[arr:[a-z0-9]+]] = alloca [2048 x <4 x float>], align 16, addrspace(5)
; SHADERTEST: {{.*}}:
; SHADERTEST: {{.*}}:
; SHADERTEST: [[check:[a-z0-9]+]]:
; SHADERTEST: %[[idx1:[0-9]+]] = load i32, {{i32 addrspace\(5\)\*|ptr addrspace\(5\)}} %{{.*}}, align 4
; SHADERTEST: %[[gep:[0-9]+]] = getelementptr [2048 x <4 x float>], {{\[2048 x <4 x float>\] addrspace\(5\)\*|ptr addrspace\(5\)}} %[[arr]], i32 0, i32 %[[idx1]]
; SHADERTEST: %[[cmp:[0-9]+]] = icmp ult i32 %[[idx1]], 2048
; SHADERTEST: br i1 %[[cmp]], label %{{.*}}, label %{{.*}}
; SHADERTEST: [[load:[a-z0-9]+]]:
; SHADERTEST: %[[loadResult:[0-9]+]] = load <4 x float>, {{<4 x float> addrspace\(5\)\*|ptr addrspace\(5\)}} %[[gep]], align 16
; SHADERTEST: br label %{{.*}}
; SHADERTEST: [[final:[a-z0-9]+]]:
; SHADERTEST: %{{.*}} = phi <4 x float> [ zeroinitializer, %[[check]] ], [ %[[loadResult]], %[[load]] ]

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450 core

layout (location = 0) in vec4 inColor;
layout (location = 0) out vec4 outFragColor;
layout (binding = 0) uniform Constant { int array_index; } c;

void main() {
  vec4 data[2048];

  for (int i = 0; i < c.array_index; ++i) {
    outFragColor += inColor * data[i];
  }
}
