#version 450 core

#define ITER 4

layout(location = 0) out vec4 frag_color;
layout(location = 0) in flat int index;
layout(location = 17) in vec4 interp[ITER];

void main()
{
   frag_color = interpolateAtOffset(interp[index], vec2(0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -verify-ir -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call {{.*}} <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: = call <3 x float> @lgc.input.import.builtin.InterpPullMode
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 17, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 18, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 19, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 20, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
