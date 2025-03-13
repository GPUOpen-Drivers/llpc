#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/


struct Struct_2
{
   float f;
   vec4 array[3][2];
};
struct Struct_1
{
   float f;
   Struct_2 s2;
};

layout(location = 0) out vec4 frag_color;
layout(location = 0) in Struct_1 interp;
layout(location = 20) in flat int x;
layout(location = 21) in flat int y;

void main()
{
    frag_color = interpolateAtOffset(interp.s2.array[x][y], vec2(0));

}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 2, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 3, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 4, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 5, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 6, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.input.import.interpolated__v4f32(i1 false, i32 7, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
