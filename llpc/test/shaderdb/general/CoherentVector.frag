#version 450
layout(set = 1, binding = 0) coherent buffer b
{
	vec4 v;
};

void main()
{
	v = vec4(42);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v --verify-ir %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: store atomic float 4.200000e+01, ptr addrspace(7) @0 unordered, align 4
; SHADERTEST: store atomic float 4.200000e+01, ptr addrspace(7) getelementptr ([4 x float], ptr addrspace(7) @0, i32 0, i32 1) unordered, align 4
; SHADERTEST: store atomic float 4.200000e+01, ptr addrspace(7) getelementptr ([4 x float], ptr addrspace(7) @0, i32 0, i32 2) unordered, align 4
; SHADERTEST: store atomic float 4.200000e+01, ptr addrspace(7) getelementptr ([4 x float], ptr addrspace(7) @0, i32 0, i32 3) unordered, align 4
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
