#version 450 core

#define ITER 3
struct Struct_2
{
   float a;
   float b;
   vec4 v;
};
struct Struct_1
{
   vec2 offset;
   Struct_2 s2;
};

layout(location = 0) out vec4 frag_color;
layout(location = 0) in Struct_1 interp[ITER];
layout(location = 20) in flat int index;

void main()
{
    frag_color = interpolateAtOffset(interp[index].s2.v, vec2(0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call {{.*}} <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: = call <3 x float> @lgc.input.import.builtin.InterpPullMode
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 3, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 7, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 11, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
