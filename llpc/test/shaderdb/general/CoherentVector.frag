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

layout(set = 1, binding = 0) coherent buffer b
{
	vec4 v;
};

void main()
{
	v = vec4(42);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v --verify-ir %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: store atomic float 4.200000e+01, ptr addrspace(7) @0 unordered, align 4
; SHADERTEST: store atomic float 4.200000e+01, ptr addrspace(7) getelementptr inbounds ([4 x float], ptr addrspace(7) @0, i32 0, i32 1) unordered, align 4
; SHADERTEST: store atomic float 4.200000e+01, ptr addrspace(7) getelementptr inbounds ([4 x float], ptr addrspace(7) @0, i32 0, i32 2) unordered, align 4
; SHADERTEST: store atomic float 4.200000e+01, ptr addrspace(7) getelementptr inbounds ([4 x float], ptr addrspace(7) @0, i32 0, i32 3) unordered, align 4
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
