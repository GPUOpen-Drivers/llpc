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


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; Test that after FE lowering all operations after first kill have been
; replaced with a branch to the return block.

; SHADERTEST-LABEL: {{^// LLPC.*}} FE lowering results
; SHADERTEST: call void{{.*}} @lgc.create.kill
; SHADERTEST-NEXT: br label %[[LABEL:[0-9]*]]
; SHADERTEST-NOT: call void{{.*}} @lgc.create.kill
; SHADERTEST: [[LABEL]]:
; SHADERTEST-NEXT: call void{{.*}} @lgc.create.write.generic.output

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

layout(location = 0) in mediump vec4 v_color1;
layout(location = 1) in mediump vec4 v_color2;
layout(location = 2) in mediump vec4 v_coords;
layout(location = 0) out mediump vec4 o_color;

void myfunc0 (void)
{
    discard;
}

void myfunc1 (void)
{
    o_color = v_color2;
}

void main (void)
{
    o_color = v_color1;
    if (v_coords.x+v_coords.y > 0.0) {
      myfunc0();
      myfunc1();
      myfunc0();
    }
}
