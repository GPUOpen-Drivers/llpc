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


layout(location = 0) in vec4 a;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 fv = radians(a);
    fv.x = radians(1.5);
    frag_color = fv;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <4 x float> %{{.*}}, {{(splat \(float 0x3F91DF46A0000000\))|(<float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000>)}}
; SHADERTEST: store float 0x3F9ACEEA00000000, ptr addrspace(5) %{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <4 x float> %{{.*}}, <float {{undef|poison}}, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000>
; SHADERTEST: = insertelement <4 x float> %{{.*}}, float 0x3F9ACEEA00000000, i{{32|64}} 0
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: [[mul1:%.i[0-9]*]] = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: [[mul2:%.i[0-9]*]] = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: [[mul3:%.i[0-9]*]] = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: float 0x3F9ACEEA00000000, float [[mul1]], float [[mul2]], float [[mul3]]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
