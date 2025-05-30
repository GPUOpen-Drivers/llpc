// NOTE: Assertions have been autogenerated by tool/update_llpc_test_checks.py UTC_ARGS: --version 5
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
// RUN: amdllpc --print-after=lgc-builder-replayer %s 2>&1 | FileCheck -check-prefix=SHADERTEST %s
#extension GL_ARB_sparse_texture_clamp : enable

layout(set = 0, binding = 0) uniform sampler1D          samp1D[4];
layout(set = 1, binding = 0) uniform sampler2D          samp2D;
layout(set = 2, binding = 0) uniform sampler3D          samp3D;
layout(set = 3, binding = 0) uniform samplerCube        sampCube;
layout(set = 4, binding = 0) uniform sampler1DArray     samp1DArray;
layout(set = 5, binding = 0) uniform sampler2DArray     samp2DArray;
layout(set = 6, binding = 0) uniform samplerCubeArray   sampCubeArray;

layout(set = 7, binding = 0) uniform Uniforms
{
    int   index;
    float lodClamp;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0);

    fragColor += textureGradOffsetClampARB(samp1D[index], 0.1, 0.2, 0.3, 2, lodClamp);

    fragColor += textureGradOffsetClampARB(samp2D, vec2(0.1), vec2(0.2), vec2(0.3), ivec2(2), lodClamp);

    fragColor += textureGradOffsetClampARB(samp3D, vec3(0.1), vec3(0.2), vec3(0.3), ivec3(2), lodClamp);

    fragColor += textureGradOffsetClampARB(samp1DArray, vec2(0.1), 0.2, 0.3, 2, lodClamp);

    fragColor += textureGradOffsetClampARB(samp2DArray, vec3(0.1), vec2(0.2), vec2(0.3), ivec2(2), lodClamp);
}
// SHADERTEST-LABEL: define dllexport spir_func void @lgc.shader.FS.main(
// SHADERTEST-SAME: ) local_unnamed_addr #[[ATTR0:[0-9]+]] !spirv.ExecutionModel [[META19:![0-9]+]] !lgc.shaderstage [[META20:![0-9]+]] {
// SHADERTEST-NEXT:  [[_ENTRY:.*:]]
// SHADERTEST-NEXT:    [[TMP0:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP1:%.*]] = bitcast i64 [[TMP0]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP2:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP3:%.*]] = bitcast i64 [[TMP2]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP4:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP5:%.*]] = bitcast i64 [[TMP4]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP6:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP7:%.*]] = bitcast i64 [[TMP6]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP8:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP9:%.*]] = bitcast i64 [[TMP8]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP10:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP11:%.*]] = bitcast i64 [[TMP10]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP12:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP13:%.*]] = bitcast i64 [[TMP12]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP14:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP15:%.*]] = bitcast i64 [[TMP14]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP16:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP17:%.*]] = bitcast i64 [[TMP16]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP18:%.*]] = call i64 @llvm.amdgcn.s.getpc()
// SHADERTEST-NEXT:    [[TMP19:%.*]] = bitcast i64 [[TMP18]] to <2 x i32>
// SHADERTEST-NEXT:    [[TMP20:%.*]] = call ptr addrspace(7) @lgc.load.buffer.desc(i64 7, i32 0, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP21:%.*]] = call ptr @llvm.invariant.start.p7(i64 -1, ptr addrspace(7) [[TMP20]])
// SHADERTEST-NEXT:    [[TMP22:%.*]] = call i32 @lgc.load.user.data__i32(i32 16)
// SHADERTEST-NEXT:    [[TMP23:%.*]] = insertelement <2 x i32> [[TMP17]], i32 [[TMP22]], i64 0
// SHADERTEST-NEXT:    [[TMP24:%.*]] = bitcast <2 x i32> [[TMP23]] to i64
// SHADERTEST-NEXT:    [[TMP25:%.*]] = inttoptr i64 [[TMP24]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP25]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP25]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP26:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP25]], i32 0
// SHADERTEST-NEXT:    [[TMP27:%.*]] = call i32 @lgc.load.user.data__i32(i32 16)
// SHADERTEST-NEXT:    [[TMP28:%.*]] = insertelement <2 x i32> [[TMP19]], i32 [[TMP27]], i64 0
// SHADERTEST-NEXT:    [[TMP29:%.*]] = bitcast <2 x i32> [[TMP28]] to i64
// SHADERTEST-NEXT:    [[TMP30:%.*]] = inttoptr i64 [[TMP29]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP30]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP30]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP31:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP30]], i32 32
// SHADERTEST-NEXT:    [[TMP32:%.*]] = call i32 @lgc.load.user.data__i32(i32 12)
// SHADERTEST-NEXT:    [[TMP33:%.*]] = insertelement <2 x i32> [[TMP13]], i32 [[TMP32]], i64 0
// SHADERTEST-NEXT:    [[TMP34:%.*]] = bitcast <2 x i32> [[TMP33]] to i64
// SHADERTEST-NEXT:    [[TMP35:%.*]] = inttoptr i64 [[TMP34]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP35]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP35]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP36:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP35]], i32 0
// SHADERTEST-NEXT:    [[TMP37:%.*]] = call i32 @lgc.load.user.data__i32(i32 12)
// SHADERTEST-NEXT:    [[TMP38:%.*]] = insertelement <2 x i32> [[TMP15]], i32 [[TMP37]], i64 0
// SHADERTEST-NEXT:    [[TMP39:%.*]] = bitcast <2 x i32> [[TMP38]] to i64
// SHADERTEST-NEXT:    [[TMP40:%.*]] = inttoptr i64 [[TMP39]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP40]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP40]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP41:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP40]], i32 32
// SHADERTEST-NEXT:    [[TMP42:%.*]] = call i32 @lgc.load.user.data__i32(i32 8)
// SHADERTEST-NEXT:    [[TMP43:%.*]] = insertelement <2 x i32> [[TMP9]], i32 [[TMP42]], i64 0
// SHADERTEST-NEXT:    [[TMP44:%.*]] = bitcast <2 x i32> [[TMP43]] to i64
// SHADERTEST-NEXT:    [[TMP45:%.*]] = inttoptr i64 [[TMP44]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP45]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP45]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP46:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP45]], i32 0
// SHADERTEST-NEXT:    [[TMP47:%.*]] = call i32 @lgc.load.user.data__i32(i32 8)
// SHADERTEST-NEXT:    [[TMP48:%.*]] = insertelement <2 x i32> [[TMP11]], i32 [[TMP47]], i64 0
// SHADERTEST-NEXT:    [[TMP49:%.*]] = bitcast <2 x i32> [[TMP48]] to i64
// SHADERTEST-NEXT:    [[TMP50:%.*]] = inttoptr i64 [[TMP49]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP50]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP50]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP51:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP50]], i32 32
// SHADERTEST-NEXT:    [[TMP52:%.*]] = call i32 @lgc.load.user.data__i32(i32 4)
// SHADERTEST-NEXT:    [[TMP53:%.*]] = insertelement <2 x i32> [[TMP5]], i32 [[TMP52]], i64 0
// SHADERTEST-NEXT:    [[TMP54:%.*]] = bitcast <2 x i32> [[TMP53]] to i64
// SHADERTEST-NEXT:    [[TMP55:%.*]] = inttoptr i64 [[TMP54]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP55]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP55]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP56:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP55]], i32 0
// SHADERTEST-NEXT:    [[TMP57:%.*]] = call i32 @lgc.load.user.data__i32(i32 4)
// SHADERTEST-NEXT:    [[TMP58:%.*]] = insertelement <2 x i32> [[TMP7]], i32 [[TMP57]], i64 0
// SHADERTEST-NEXT:    [[TMP59:%.*]] = bitcast <2 x i32> [[TMP58]] to i64
// SHADERTEST-NEXT:    [[TMP60:%.*]] = inttoptr i64 [[TMP59]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP60]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP60]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP61:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP60]], i32 32
// SHADERTEST-NEXT:    [[TMP62:%.*]] = call i32 @lgc.load.user.data__i32(i32 0)
// SHADERTEST-NEXT:    [[TMP63:%.*]] = insertelement <2 x i32> [[TMP1]], i32 [[TMP62]], i64 0
// SHADERTEST-NEXT:    [[TMP64:%.*]] = bitcast <2 x i32> [[TMP63]] to i64
// SHADERTEST-NEXT:    [[TMP65:%.*]] = inttoptr i64 [[TMP64]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP65]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP65]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP66:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP65]], i32 0
// SHADERTEST-NEXT:    [[TMP67:%.*]] = call i32 @lgc.load.user.data__i32(i32 0)
// SHADERTEST-NEXT:    [[TMP68:%.*]] = insertelement <2 x i32> [[TMP3]], i32 [[TMP67]], i64 0
// SHADERTEST-NEXT:    [[TMP69:%.*]] = bitcast <2 x i32> [[TMP68]] to i64
// SHADERTEST-NEXT:    [[TMP70:%.*]] = inttoptr i64 [[TMP69]] to ptr addrspace(4)
// SHADERTEST-NEXT:    call void @llvm.assume(i1 true) [ "align"(ptr addrspace(4) [[TMP70]], i32 4), "dereferenceable"(ptr addrspace(4) [[TMP70]], i32 -1) ]
// SHADERTEST-NEXT:    [[TMP71:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP70]], i32 32
// SHADERTEST-NEXT:    [[TMP72:%.*]] = load i32, ptr addrspace(7) [[TMP20]], align 4
// SHADERTEST-NEXT:    [[TMP73:%.*]] = mul i32 [[TMP72]], 48
// SHADERTEST-NEXT:    [[TMP74:%.*]] = sext i32 [[TMP73]] to i64
// SHADERTEST-NEXT:    [[TMP75:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP66]], i64 [[TMP74]]
// SHADERTEST-NEXT:    [[TMP76:%.*]] = mul i32 [[TMP72]], 48
// SHADERTEST-NEXT:    [[TMP77:%.*]] = sext i32 [[TMP76]] to i64
// SHADERTEST-NEXT:    [[TMP78:%.*]] = getelementptr i8, ptr addrspace(4) [[TMP71]], i64 [[TMP77]]
// SHADERTEST-NEXT:    [[TMP79:%.*]] = getelementptr i8, ptr addrspace(7) [[TMP20]], i32 4
// SHADERTEST-NEXT:    [[TMP80:%.*]] = load float, ptr addrspace(7) [[TMP79]], align 4
// SHADERTEST-NEXT:    [[TMP81:%.*]] = load <8 x i32>, ptr addrspace(4) [[TMP75]], align 4, !invariant.load [[META21:![0-9]+]]
// SHADERTEST-NEXT:    [[TMP82:%.*]] = call <8 x i32> @llvm.amdgcn.readfirstlane.v8i32(<8 x i32> [[TMP81]])
// SHADERTEST-NEXT:    [[TMP83:%.*]] = load <4 x i32>, ptr addrspace(4) [[TMP78]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP84:%.*]] = call <4 x i32> @llvm.amdgcn.readfirstlane.v4i32(<4 x i32> [[TMP83]])
// SHADERTEST-NEXT:    [[TMP85:%.*]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.d.cl.o.1d.v4f32.f32.f32.v8i32.v4i32(i32 15, i32 2, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float [[TMP80]], <8 x i32> [[TMP82]], <4 x i32> [[TMP84]], i1 false, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP86:%.*]] = load <8 x i32>, ptr addrspace(4) [[TMP56]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP87:%.*]] = load <4 x i32>, ptr addrspace(4) [[TMP61]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP88:%.*]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.d.cl.o.2d.v4f32.f32.f32.v8i32.v4i32(i32 15, i32 514, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float [[TMP80]], <8 x i32> [[TMP86]], <4 x i32> [[TMP87]], i1 false, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP89:%.*]] = fadd reassoc nnan nsz arcp contract afn <4 x float> [[TMP85]], [[TMP88]]
// SHADERTEST-NEXT:    [[TMP90:%.*]] = load <8 x i32>, ptr addrspace(4) [[TMP46]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP91:%.*]] = load <4 x i32>, ptr addrspace(4) [[TMP51]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP92:%.*]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.d.cl.o.3d.v4f32.f32.f32.v8i32.v4i32(i32 15, i32 131586, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float [[TMP80]], <8 x i32> [[TMP90]], <4 x i32> [[TMP91]], i1 false, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP93:%.*]] = fadd reassoc nnan nsz arcp contract afn <4 x float> [[TMP89]], [[TMP92]]
// SHADERTEST-NEXT:    [[TMP94:%.*]] = load <8 x i32>, ptr addrspace(4) [[TMP36]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP95:%.*]] = load <4 x i32>, ptr addrspace(4) [[TMP41]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP96:%.*]] = call reassoc nnan nsz arcp contract afn float @llvm.rint.f32(float 0x3FB99999A0000000)
// SHADERTEST-NEXT:    [[TMP97:%.*]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.d.cl.o.1darray.v4f32.f32.f32.v8i32.v4i32(i32 15, i32 2, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float [[TMP96]], float [[TMP80]], <8 x i32> [[TMP94]], <4 x i32> [[TMP95]], i1 false, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP98:%.*]] = fadd reassoc nnan nsz arcp contract afn <4 x float> [[TMP93]], [[TMP97]]
// SHADERTEST-NEXT:    [[TMP99:%.*]] = load <8 x i32>, ptr addrspace(4) [[TMP26]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP100:%.*]] = load <4 x i32>, ptr addrspace(4) [[TMP31]], align 4, !invariant.load [[META21]]
// SHADERTEST-NEXT:    [[TMP101:%.*]] = call reassoc nnan nsz arcp contract afn float @llvm.rint.f32(float 0x3FB99999A0000000)
// SHADERTEST-NEXT:    [[TMP102:%.*]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.d.cl.o.2darray.v4f32.f32.f32.v8i32.v4i32(i32 15, i32 514, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FD3333340000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float [[TMP101]], float [[TMP80]], <8 x i32> [[TMP99]], <4 x i32> [[TMP100]], i1 false, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP103:%.*]] = fadd reassoc nnan nsz arcp contract afn <4 x float> [[TMP98]], [[TMP102]]
// SHADERTEST-NEXT:    call void @lgc.output.export.generic.i32.i32.v4f32(i32 0, i32 0, <4 x float> [[TMP103]]) #[[ATTR7:[0-9]+]]
// SHADERTEST-NEXT:    ret void
//
//.
// SHADERTEST: [[META19]] = !{i32 4}
// SHADERTEST: [[META20]] = !{i32 6}
// SHADERTEST: [[META21]] = !{}
//.
