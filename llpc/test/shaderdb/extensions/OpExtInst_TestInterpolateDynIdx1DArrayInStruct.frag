#version 450 core

#define ITER 5

struct Struct_2
{
   float f1;
   vec4 array[ITER];
};
struct Struct_1
{
   float f1;
   Struct_2 s2;
};

layout(location = 0) out vec4 frag_color;
layout(location = 0) in flat int index;
layout(location = 1) in Struct_1 interp;

void main()
{
    frag_color = interpolateAtOffset(interp.s2.array[index], vec2(0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call {{.*}} <4 x float> @interpolateAtOffset.v4f32.p64.v2f32({{<4 x float> addrspace\(64\)\*|ptr addrspace\(64\)}} %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: = call <3 x float> @lgc.input.import.builtin.InterpPullMode
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated.v4f32(i1 false, i32 3, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated.v4f32(i1 false, i32 4, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated.v4f32(i1 false, i32 5, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated.v4f32(i1 false, i32 6, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST-DAG: %{{[0-9]*}} = call <4 x float> (...) @lgc.input.import.interpolated.v4f32(i1 false, i32 7, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
