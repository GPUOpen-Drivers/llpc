#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


struct Struct_2
{
   float b;
   vec4 array[3][2];
   float c;
};
struct Struct_1
{
   float f;
   Struct_2 s2;
};

layout(location = 0) out vec4 frag_color;
layout(location = 10) in Struct_1 interp[2];
layout(location = 28) in flat int x;
layout(location = 29) in flat int y;
layout(location = 30) in flat int z;


void main()
{
    frag_color = interpolateAtOffset(interp[x].s2.array[y][z], vec2(0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 12, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 13, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 14, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 15, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 16, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 17, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 21, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 22, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 23, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 24, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 25, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 26, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
