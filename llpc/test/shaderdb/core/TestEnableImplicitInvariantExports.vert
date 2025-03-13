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

// If implicit invariant marking is allowed for instructions contributing to gl_Position exports, the
// fast math flag is disabled for these instructions. This occurs if invariance is expected even if no
// invariance flag is being used in SPIR-V. Enabling the FMF can sometimes break rendering with FMA
// instructions being generated.
// Check if FMF for position exports is disabled if --enable-implicit-invariant-exports is set to 1.

#version 450

layout(location = 0) in vec4 inPos;

layout(std140, binding = 0) uniform block {
    uniform mat4 mvp;
};

void main()
{
    gl_Position = mvp * inPos;
}

// BEGIN_WITHOUT_IIE
/*
; RUN: amdllpc -v --enable-implicit-invariant-exports=1 %gfxip %s | FileCheck -check-prefix=WITHOUT_IIE %s
; WITHOUT_IIE-LABEL: {{^// LLPC}} LGC before-lowering results
; WITHOUT_IIE: %[[val:.*]] = extractvalue [4 x <4 x float>] %{{.*}}, 3
; WITHOUT_IIE: %[[mul:.*]] = fmul nnan nsz afn <4 x float> %[[val]], %{{.*}}
; WITHOUT_IIE: %[[arg:.*]] = fadd nnan nsz afn <4 x float> %{{.*}}, %[[mul]]
; WITHOUT_IIE-NEXT: call void @lgc.output.export.builtin.Position.i32.v4f32(i32 0, <4 x float> %[[arg]])
; WITHOUT_IIE: AMDLLPC SUCCESS
*/
// END_WITHOUT_IIE

// BEGIN_WITH_IIE
/*
; RUN: amdllpc -v --enable-implicit-invariant-exports=0 %s | FileCheck -check-prefix=WITH_IIE %s
; WITH_IIE-LABEL: {{^// LLPC}} LGC before-lowering results
; WITH_IIE: %[[val:.*]] = extractvalue [4 x <4 x float>] %{{.*}}, 3
; WITH_IIE: %[[mul:.*]] = fmul reassoc nnan nsz arcp contract afn <4 x float> %[[val]], %{{.*}}
; WITH_IIE: %[[arg:.*]] = fadd reassoc nnan nsz arcp contract afn <4 x float> %{{.*}}, %[[mul]]
; WITH_IIE-NEXT: call void @lgc.output.export.builtin.Position.i32.v4f32(i32 0, <4 x float> %[[arg]])
; WITH_IIE: AMDLLPC SUCCESS
*/
// END_WITH_IIE
