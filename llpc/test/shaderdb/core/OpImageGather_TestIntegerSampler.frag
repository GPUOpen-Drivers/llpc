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
    const ivec2 temp[4] = {{1, 1}, {2, 2}, {3, 3}, {4, 4}};
    oColor1  = textureGather(iSamp2D, vec2(0, 1), 0);
    oColor1 += textureGatherOffset(iSamp2D, vec2(0, 1), ivec2(1, 2), 0);
    oColor1 += textureGatherOffsets(iSamp2D, vec2(0, 1), temp, 0);
    oColor2  = textureGather(uSamp2D, vec2(0, 1), 0);
    oColor2 += textureGatherOffset(uSamp2D, vec2(0, 1), ivec2(1, 2), 0);
    oColor2 += textureGatherOffsets(uSamp2D, vec2(0, 1), temp, 0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 2, i32 2, i64 0, i32 1
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 2, i32 2, i64 0, i32 0
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 516, ptr addrspace(4) {{.*}}, ptr addrspace(4) {{.*}}, i32 37, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 516, ptr addrspace(4) {{.*}}, ptr addrspace(4) {{.*}}, i32 293, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, <2 x i32> <i32 1, i32 2>)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 516, ptr addrspace(4) {{.*}}, ptr addrspace(4) {{.*}}, i32 293, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, [4 x <2 x i32>] [<2 x i32> {{(splat \(i32 1\))|(<i32 1, i32 1>)}}, <2 x i32> {{(splat \(i32 2\))|(<i32 2, i32 2>)}}, <2 x i32> {{(splat \(i32 3\))|(<i32 3, i32 3>)}}, <2 x i32> {{(splat \(i32 4\))|(<i32 4, i32 4>)}}])
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 512, ptr addrspace(4) {{.*}}, ptr addrspace(4) {{.*}}, i32 37, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 512, ptr addrspace(4) {{.*}}, ptr addrspace(4) {{.*}}, i32 293, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, <2 x i32> <i32 1, i32 2>)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 512, ptr addrspace(4) {{.*}}, ptr addrspace(4) {{.*}}, i32 293, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, [4 x <2 x i32>] [<2 x i32> {{(splat \(i32 1\))|(<i32 1, i32 1>)}}, <2 x i32> {{(splat \(i32 2\))|(<i32 2, i32 2>)}}, <2 x i32> {{(splat \(i32 3\))|(<i32 3, i32 3>)}}, <2 x i32> {{(splat \(i32 4\))|(<i32 4, i32 4>)}}])

; SHADERTEST-LABEL: {{^// LLPC}}  LGC lowering results
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 513, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 257, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 514, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 771, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 1028, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 513, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 257, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 514, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 771, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f16{{(\.v8i32)?}}{{(\.v4i32)?}}(i32 1, i32 1028, half 0xH0000, half 0xH3C00, <8 x i32> %{{.*}}, <4 x i32> %{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
