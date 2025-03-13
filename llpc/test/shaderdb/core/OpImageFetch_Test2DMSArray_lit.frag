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


layout(set = 0, binding = 0) uniform sampler2DMSArray samp;
layout(location = 0) in vec3 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    ivec3 iUV = ivec3(inUV);
    oColor = texelFetch(samp, iUV, 2);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results


call { <8 x i32> addrspace(4)*, i32 } (...) @"lgc.create.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
call { <8 x i32> addrspace(4)*, i32 } (...) @"lgc.create.get.fmask.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
call { <4 x i32> addrspace(4)*, i32 } (...) @"lgc.create.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 0, i32 0)
call <4 x float> (...) @lgc.create.image.load.with.fmask.v4f32(i32 7, i32 0,{{.*}}, i32 2)

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
call i32 @llvm.amdgcn.image.load.3d.i32.i32{{(\.v8i32)?}}(i32 1,{{.*}}, i32 0, i32 0)
call <4 x float> @llvm.amdgcn.image.load.2darraymsaa.v4f32.i32{{(\.v8i32)?}}(i32 15, i32 {{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
