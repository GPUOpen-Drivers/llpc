/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

// This test case checks whether the emit instruction and the primitive type are well-handled when there is no output
// of the geometry shader.
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-COUNT-2: call void (...) @lgc.create.emit.vertex(i32 0)
; SHADERTEST-LABEL: _amdgpu_gs_main:
; SHADERTEST-COUNT-2: s_sendmsg sendmsg(MSG_GS, GS_OP_EMIT, 0)
; SHADERTEST-LABEL: PalMetadata
; SHADERTEST-LABEL: .graphics_registers:
; SHADERTEST:         .vgt_gs_out_prim_type:
; SHADERTEST:         .outprim_type:   LineStrip
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450
layout(lines) in;
layout(max_vertices = 2, line_strip) out;

void main()
{
    EmitVertex();
    EmitVertex();
    EndPrimitive();
}
