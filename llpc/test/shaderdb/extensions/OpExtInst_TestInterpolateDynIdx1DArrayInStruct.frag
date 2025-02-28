#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


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
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @interpolateAtOffset.v4f32.p64.v2f32({{<4 x float> addrspace\(64\)\*|ptr addrspace\(64\)}} %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST-COUNT-5: call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.eval.Ij.offset.smooth__v2f32(<2 x float> zeroinitializer)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
