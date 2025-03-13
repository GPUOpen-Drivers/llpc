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


layout(set = 0, binding = 0) buffer INPUT
{
    int   imem;
    uint  umem[4];
};

layout(set = 0, binding = 1) buffer OUTPUT
{
    uvec4 u4;
};

layout(set = 1, binding = 0) uniform Uniforms
{
    int   idata;
    uint  udata;

    int index;
};

void main()
{
    int i1 = atomicAdd(imem, idata);
    i1 += atomicMin(imem, idata);
    i1 += atomicMax(imem, idata);
    i1 += atomicAnd(imem, idata);
    i1 += atomicOr(imem, idata);
    i1 += atomicXor(imem, idata);
    i1 += atomicExchange(imem, idata);
    i1 += atomicCompSwap(imem, 28, idata);

    uint u1 = atomicAdd(umem[0], udata);
    u1 += atomicMin(umem[1], udata);
    u1 += atomicMax(umem[2], udata);
    u1 += atomicAnd(umem[3], udata);
    u1 += atomicOr(umem[index], udata);
    u1 += atomicXor(umem[index], udata);
    u1 += atomicExchange(umem[index], udata);
    u1 += atomicCompSwap(umem[index], 16u, udata);

    u4[i1 % 4] = u1;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: atomicrmw add ptr {{.*}} monotonic
; SHADERTEST: atomicrmw min ptr {{.*}} monotonic
; SHADERTEST: atomicrmw max ptr {{.*}} monotonic
; SHADERTEST: atomicrmw and ptr {{.*}} monotonic
; SHADERTEST: atomicrmw or ptr {{.*}} monotonic
; SHADERTEST: atomicrmw xor ptr {{.*}} monotonic
; SHADERTEST: atomicrmw xchg ptr {{.*}} monotonic
; SHADERTEST: cmpxchg ptr {{.*}} monotonic monotonic
; SHADERTEST: atomicrmw add ptr {{.*}} monotonic
; SHADERTEST: atomicrmw umin ptr {{.*}} monotonic
; SHADERTEST: atomicrmw umax ptr {{.*}} monotonic
; SHADERTEST: atomicrmw and ptr {{.*}} monotonic
; SHADERTEST: atomicrmw or ptr {{.*}} monotonic
; SHADERTEST: atomicrmw xor ptr {{.*}} monotonic
; SHADERTEST: atomicrmw xchg ptr {{.*}} monotonic
; SHADERTEST: cmpxchg ptr {{.*}} monotonic monotonic

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.add.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.smin.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.smax.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.and.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.or.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.xor.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.swap.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.cmpswap.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, i32 28, <4 x i32> %{{[0-9a-z.]*}}, i32 0, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.add.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 4, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.umin.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 8, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.umax.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 12, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.and.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 16, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.or.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 %{{[0-9a-z.]*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.xor.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 %{{[0-9a-z.]*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.swap.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 %{{[0-9a-z.]*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.atomic.cmpswap.i32{{(\.v4i32)?}}(i32 %{{[0-9]*}}, i32 16, <4 x i32> %{{[0-9]*}}, i32 %{{[0-9a-z.]*}}, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
