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


layout(set=0, binding=0) uniform sampler2D s0;
layout(set=0, binding=1) uniform sampler2D s1;
layout(set=0, binding=2) uniform sampler2D s2;
layout(set=0, binding=3) uniform sampler2D s3;
layout(set=0, binding=4) uniform sampler2D s4;
layout(set=0, binding=5) uniform sampler2D s5;
layout(set=0, binding=6) uniform sampler2D s6;
layout(set=0, binding=7) uniform sampler2D s7;
layout(set=0, binding=8) uniform sampler2D s8;
layout(set=0, binding=9) uniform sampler2D s9;
layout(set=0, binding=10) uniform sampler2D s10;
layout(set=0, binding=11) uniform sampler2D s11;
layout(set=0, binding=12) uniform sampler2D s12;
layout(set=0, binding=13) uniform sampler2D s13;
layout(set=0, binding=14) uniform sampler2D s14;
layout(set=0, binding=15) uniform sampler2D s15;
layout(set=0, binding=16) uniform sampler2D s16;

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = texture(s0, vec2(0)) +
                texture(s1, vec2(0)) +
                texture(s2, vec2(0)) +
                texture(s3, vec2(0)) +
                texture(s4, vec2(0)) +
                texture(s5, vec2(0)) +
                texture(s6, vec2(0)) +
                texture(s7, vec2(0)) +
                texture(s8, vec2(0)) +
                texture(s9, vec2(0)) +
                texture(s10, vec2(0)) +
                texture(s11, vec2(0)) +
                texture(s12, vec2(0)) +
                texture(s13, vec2(0)) +
                texture(s14, vec2(0)) +
                texture(s15, vec2(0)) +
                texture(s16, vec2(0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC.*}} FE lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, ptr addrspace(4) %{{[-0-9A-Za0z_.]+}}, i32 1, <2 x float> zeroinitializer)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
