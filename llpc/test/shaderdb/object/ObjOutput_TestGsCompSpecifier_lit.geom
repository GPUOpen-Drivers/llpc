#version 450 core
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
layout(triangle_strip, max_vertices = 4) out;

layout(location = 0, component = 1) out vec3 f3;
layout(location = 0, component = 0) out float f1;

void main()
{
    for (int i = 0; i < gl_in.length(); ++i)
    {
        f3 = vec3(3.0);
        f1 = 1.5;
        EmitVertex();
    }

    EndPrimitive();
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v3f32(i32 0, i32 1, i32 0, <3 x float> {{(splat \(float 3\.000000e\+00\))|(<float 3\.000000e\+00, float 3\.000000e\+00, float 3\.000000e\+00>)}})
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f32(i32 0, i32 0, i32 0, float 1.500000e+00)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
