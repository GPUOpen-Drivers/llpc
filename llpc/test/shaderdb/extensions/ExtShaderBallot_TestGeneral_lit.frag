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


#extension GL_ARB_shader_ballot: enable
#extension GL_ARB_gpu_shader_int64: enable

layout(location = 0) in flat int i1;
layout(location = 1) in float f1;

layout(location = 0) out float f;

void main(void)
{
    uint64_t u64 = 0;

    u64 += gl_SubGroupInvocationARB;
    u64 += gl_SubGroupSizeARB;
    u64 += gl_SubGroupEqMaskARB;
    u64 += gl_SubGroupGeMaskARB;
    u64 += gl_SubGroupGtMaskARB;
    u64 += gl_SubGroupLeMaskARB;
    u64 += gl_SubGroupLtMaskARB;

    u64 += ballotARB(true);

    f = float(u64);

    f += float(readInvocationARB(i1, 1));
    f += float(readFirstInvocationARB(i1));

    f += readInvocationARB(f1, 2);
    f += readFirstInvocationARB(f1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call <4 x i32> (...) @lgc.create.subgroup.ballot.v4i32(
; SHADERTEST: call i32 (...) @lgc.create.subgroup.broadcast.i32(
; SHADERTEST: call i32 (...) @lgc.create.subgroup.broadcast.first.i32(
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.subgroup.broadcast.f32(
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.subgroup.broadcast.first.f32(
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: mbcnt_lo

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
