#version 450
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


layout (std140, binding = 1) uniform BB1
{
    vec4 c1;
};

layout (std140, binding = 1) uniform BB2
{
    vec4 c2;
};

layout (binding = 2) uniform sampler2D s1;
layout (binding = 2) uniform sampler2D s2;

layout (rgba8, binding = 3) uniform image2D i1;
layout (rgba8, binding = 3) uniform image2D i2;

layout (std430, binding = 4) buffer SB1
{
    vec4 sb1;
    vec4 sb2;
};

layout (std430, binding = 4) buffer SB2
{
    vec4 sb_1;
    vec4 sb_2;
};

layout (location = 0) out vec4 frag_color;
void main()
{
    frag_color = c1 + c2;
    sb1 = texture(s1, vec2(0)) + texture(s2, vec2(0));
    sb_2 = imageLoad(i1, ivec2(0)) + imageLoad(i2, ivec2(0));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 512, ptr addrspace(4)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 512, ptr addrspace(4)

; SHADERTEST-LABEL: {{^// LLPC.*}} FE lowering results
; SHADERTEST: call {{.*}} {{.*}}@lgc.load.buffer.desc{{.*}}(i64 0, i32 1,{{.*}}
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 512, ptr addrspace(4)


; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
