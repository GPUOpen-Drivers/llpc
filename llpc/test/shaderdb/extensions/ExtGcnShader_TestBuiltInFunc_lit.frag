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


#extension GL_AMD_gcn_shader: enable

#extension GL_ARB_gpu_shader_int64: enable

layout(location = 0) in vec3 f3;
layout(location = 1) out vec4 f4;

void main()
{
    float f1 = cubeFaceIndexAMD(f3);
    vec2 f2 = cubeFaceCoordAMD(f3);

    uint64_t u64 = timeAMD();

    f4.x = f1;
    f4.yz = f2;
    f4.w = float(u64);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.cube.face.index.f32(<3 x float>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.create.cube.face.coord.v2f32(<3 x float>
; SHADERTEST: = call i64 (...) @lgc.create.read.clock.i64(i1 false)
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubeid
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubema
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubesc
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubetc
; SHADERTEST: [[TIME:%[^ ]*]] = call i64 @llvm.readcyclecounter()
; SHADERTEST: = call i64 asm sideeffect "; %1", "=r,0"(i64 [[TIME]])
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
