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


layout(binding = 0) uniform Uniforms
{
    int i1_3;
    ivec3 i3_1;

    int i1_1, i1_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1_0 = bitfieldExtract(i1_3, i1_1, i1_2);

    ivec3 i3_0 = bitfieldExtract(i3_1, i1_1, i1_2);

    fragColor = (i1_0 != i3_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call i32 (...) @lgc.create.extract.bit.field.i32(i32 {{.*}}, i1 true)
; SHADERTEST: call <3 x i32> (...) @lgc.create.extract.bit.field.v3i32(<3 x i32> {{.*}}, i1 true)

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST-CONUT-2: call i32 @llvm.amdgcn.sbfe.i32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
