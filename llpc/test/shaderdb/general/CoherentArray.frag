#version 450
layout(set = 1, binding = 0) buffer coherent b
{
	vec4 v[3];
};

void main()
{
	v = vec4[3](vec4(42), vec4(42), vec4(42));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v --verify-ir %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 0, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 4, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 8, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 12, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 16, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 20, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 24, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 28, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 32, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 36, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 40, i32 0, i32 1)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32{{(\.v4i32)?}}(i32 1109917696, <4 x i32> %5, i32 44, i32 0, i32 1)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
