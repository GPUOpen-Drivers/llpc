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


layout(std140, push_constant) uniform PushConstant
{
   vec4 m1;
   vec4 m2;
   vec4 m3;
   vec4 m4;
   vec4 m5;
   vec4 m6;
   vec4 m7;
   vec4 m8;
   vec4 m9;
   vec4 m10;
   vec4 m11;
   vec4 m12;
   vec4 m13;
   vec4 m14;
   vec4 m15;
   vec4 m16;
   vec4 m17;
   vec4 m18;
   vec4 m19;
   vec4 m20;
} pushConst;

layout(location = 0) out vec4 o1;

void main()
{
    o1 = pushConst.m5 + pushConst.m10;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} FE lowering results
; SHADERTEST:  [[V0:%.*]] = call {{.*}} @lgc.create.load.push.constants.ptr
; SHADERTEST:  [[V1:%.*]] = getelementptr {{.*}} addrspace(4) [[V0]], i64 {{64|0, i32 4}}
; SHADERTEST:  load <4 x float>, ptr addrspace(4) [[V1]], align 16
; SHADERTEST:  [[V11:%.*]] = getelementptr {{.*}} addrspace(4) [[V0]], i64 {{64|0, i32 4}}
; SHADERTEST:  load <4 x float>, ptr addrspace(4) [[V11]], align 16

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
