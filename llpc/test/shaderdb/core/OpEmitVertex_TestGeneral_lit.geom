#version 450
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


layout(triangles) in;
layout(triangle_strip, max_vertices=32) out;

layout(location = 0) in vec4 colorIn[];
layout(location = 6) out vec4 colorOut;

void main ( )
{
    for (int i = 0; i < gl_in.length(); i++)
    {
        colorOut = colorIn[i];
        EmitVertex();
    }

    EndPrimitive();
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{[a-zA-Z_]+}} void @EmitVertex()
; SHADERTEST: call void (...) @lgc.create.end.primitive(i32 0)
; SHADERTEST-LABEL: {{^// LLPC.*}} LGC lowering results
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 34, i32 %gsWaveId)
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 34, i32 %gsWaveId)
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 34, i32 %gsWaveId)
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 18, i32 %gsWaveId)
; SHADERTEST: call void @llvm.amdgcn.s.sendmsg(i32 3, i32 %gsWaveId)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
