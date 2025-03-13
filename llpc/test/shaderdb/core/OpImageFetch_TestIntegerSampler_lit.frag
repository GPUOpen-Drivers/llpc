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


layout(set = 0, binding = 0) uniform isampler2D iSamp2D;
layout(set = 0, binding = 1) uniform usampler2D uSamp2D;
layout(location = 0) out ivec4 oColor1;
layout(location = 1) out uvec4 oColor2;

void main()
{
    oColor1 = texelFetch(iSamp2D, ivec2(0, 1), 0);
    oColor2 = texelFetch(uSamp2D, ivec2(0, 1), 0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 1, i32 1540, {{.*}}, <2 x i32> <i32 0, i32 1>, i32 0)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 1, i32 1536, {{.*}}, <2 x i32> <i32 0, i32 1>, i32 0)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.load.2d.v4i32.i16{{(\.v8i32)?}}(i32 15, i16 0, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0), !invariant.load !{{.*}}
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.load.2d.v4i32.i16{{(\.v8i32)?}}(i32 15, i16 0, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0), !invariant.load !{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
