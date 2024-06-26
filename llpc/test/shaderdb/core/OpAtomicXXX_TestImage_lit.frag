#version 450

layout(set = 0, binding = 0, r32i)  uniform iimage1D        iimg1D;
layout(set = 1, binding = 0, r32i)  uniform iimage2D        iimg2D[4];
layout(set = 0, binding = 1, r32i)  uniform iimage2DMS      iimg2DMS;
layout(set = 0, binding = 2, r32ui) uniform uimageCube      uimgCube;
layout(set = 2, binding = 0, r32ui) uniform uimageBuffer    uimgBuffer[4];
layout(set = 0, binding = 3, r32ui) uniform uimage2DMSArray uimg2DMSArray;
layout(set = 0, binding = 4, r32f)  uniform image2DRect     img2DRect;

layout(set = 3, binding = 0) uniform Uniforms
{
    int   idata;
    uint  udata;
    float fdata;

    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1 = imageAtomicAdd(iimg1D, 1, idata);
    i1 += imageAtomicMin(iimg2D[1], ivec2(2), idata);
    i1 += imageAtomicMax(iimg2D[index], ivec2(2), idata);
    i1 += imageAtomicAnd(iimg2DMS, ivec2(2), 4, idata);
    i1 += imageAtomicOr(iimg1D, 1, idata);
    i1 += imageAtomicXor(iimg1D, 2, idata);
    i1 += imageAtomicExchange(iimg1D, 1, idata);
    i1 += imageAtomicCompSwap(iimg1D, 1, 28, idata);

    uint u1 = imageAtomicAdd(uimgCube, ivec3(1), udata);
    u1 += imageAtomicMin(uimgBuffer[1], 2, udata);
    u1 += imageAtomicMax(uimgBuffer[index], 1, udata);
    u1 += imageAtomicAnd(uimg2DMSArray, ivec3(2), 5, udata);
    u1 += imageAtomicOr(uimgCube, ivec3(1), udata);
    u1 += imageAtomicXor(uimgCube, ivec3(2), udata);
    u1 += imageAtomicExchange(uimgCube, ivec3(1), udata);
    u1 += imageAtomicCompSwap(uimgCube, ivec3(1), 17u, udata);

    float f1 = imageAtomicExchange(img2DRect, ivec2(3), fdata);

    fragColor = vec4(i1, i1, u1, f1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 2, i32 0, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 4, i32 1, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 6, i32 1, i32 128, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 8, i32 6, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 9, i32 0, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 10, i32 0, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 0, i32 0, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.compare.swap.i32(i32 0, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 2, i32 3, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 5, i32 10, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 7, i32 10, i32 128, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 8, i32 7, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 9, i32 3, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 10, i32 3, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.i32(i32 0, i32 3, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call i32 (...) @lgc.create.image.atomic.compare.swap.i32(i32 3, i32 0, i32 0, ptr addrspace(4)
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.image.atomic.f32(i32 0, i32 9, i32 0, i32 0, ptr addrspace(4)

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.add.1d.i32.i16(i32 %{{.*}}, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.smin.2d.i32.i16(i32 %{{.*}}, i16 2, i16 2, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.smax.2d.i32.i16(i32 %{{.*}}, i16 2, i16 2, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.and.2dmsaa.i32.i16(i32 %{{.*}}, i16 2, i16 2, i16 4, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.or.1d.i32.i16(i32 %{{.*}}, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.xor.1d.i32.i16(i32 %{{.*}}, i16 2, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.swap.1d.i32.i16(i32 %{{.*}}, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.cmpswap.1d.i32.i16(i32 %{{.*}}, i32 28, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.add.cube.i32.i16(i32 %{{.*}}, i16 1, i16 1, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.struct.buffer.atomic.umin.i32(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 2, i32 0, i32 0
; SHADERTEST: call i32 @llvm.amdgcn.struct.buffer.atomic.umax.i32(i32 %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 1, i32 0, i32 0
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.and.2darraymsaa.i32.i16(i32 %{{.*}}, i16 2, i16 2, i16 2, i16 5, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.or.cube.i32.i16(i32 %{{.*}}, i16 1, i16 1, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.xor.cube.i32.i16(i32 %{{.*}}, i16 2, i16 2, i16 2, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.swap.cube.i32.i16(i32 %{{.*}}, i16 1, i16 1, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call i32 @llvm.amdgcn.image.atomic.cmpswap.cube.i32.i16(i32 %{{.*}}, i32 17, i16 1, i16 1, i16 1, <8 x i32> %{{.*}}, i32 0, i32 0)
; SHADERTEST: call {{.*}} float @llvm.amdgcn.image.atomic.swap.2d.f32.i32(float %{{[-0-9A-Za0z_.]+}}, i32 3, i32 3, <8 x i32> %{{[-0-9A-Za0z_.]+}}, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
