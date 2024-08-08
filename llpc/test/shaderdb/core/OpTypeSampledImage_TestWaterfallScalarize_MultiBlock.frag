#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 7) uniform sampler2D _11[];

layout(location = 0) out vec4 _3;
layout(location = 1) flat in int _4;
layout(location = 2) flat in int _4b;
layout(location = 3) flat in vec2 _6;
layout(location = 4) flat in vec2 _7;

void main()
{
    int _12 = _4;
    vec2 coord2 = _7;
    vec4 samp0 = texture(_11[nonuniformEXT(_12)], _6);
    vec4 samp1 = texture(_11[nonuniformEXT(_12)], coord2);
    if (_4b == 0) {
      coord2 = coord2 * 2;
      samp1 = texture(_11[nonuniformEXT(_12)], coord2);
    }
    _3 = samp0 + samp1;
}

// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v %gfxip %s | FileCheck -check-prefix=SHADERTEST-GFX %s
// Explicitly check GFX10.3 ASIC variants:
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v --gfxip=10.3.0 %s | FileCheck -check-prefix=SHADERTEST-GFX_10_3_0 %s
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v --gfxip=10.3.2 %s | FileCheck -check-prefix=SHADERTEST-GFX_10_3_2 %s
// Make sure that there are two non-overlapping waterfall loops
// Make sure that both the image resource desc and sample desc have the same index and there is only one
// waterfall.readfirstlane for both of them

// SHADERTEST-GFX-LABEL: {{^// LLPC}} pipeline patching results
// SHADERTEST-GFX: %[[mul1:[0-9]+]] = mul i32 %{{.*}}, 48
// SHADERTEST-GFX-NEXT: %[[begin1:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX-NEXT: %[[readfirstlane1:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin1]], i32 %[[mul1]])
// SHADERTEST-GFX-NEXT: %[[sext1:[0-9]+]] = sext i32 %[[readfirstlane1]] to i64
// SHADERTEST-GFX-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext1]]
// SHADERTEST-GFX-NEXT: %[[load1:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep1]], align 4
// SHADERTEST-GFX-NEXT: %[[extract1:[.a-z0-9]+]] = extractelement <8 x i32> %[[load1]], i64 3
// SHADERTEST-GFX-NEXT: %[[and1:[0-9]+]] = and i32 %[[extract1]], 268435455
// SHADERTEST-GFX-NEXT: %[[cmp1:[0-9]+]] = icmp slt i32 %[[extract1]], 0
// SHADERTEST-GFX-NEXT: %[[select1:[0-9]+]] = select i1 %[[cmp1]], i32 %[[extract1]], i32 %[[and1]]
// SHADERTEST-GFX-NEXT: %[[insert1:[.a-z0-9]+]] = insertelement <8 x i32> %[[load1]], i32 %[[select1]], i64 3
// SHADERTEST-GFX-NEXT: %[[shufflevector1:[0-9]+]] = shufflevector <8 x i32> %[[insert1]], <8 x i32> %[[load1]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 12, i32 13, i32 14, i32 15>
// SHADERTEST-GFX-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext1]]
// SHADERTEST-GFX-NEXT: %[[load2:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep2]], align 4
// SHADERTEST-GFX-NEXT: %[[image_call1:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector1]], <4 x i32> %[[load2]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX-NEXT: %[[end1:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin1]], <4 x float> %[[image_call1]])
//
// SHADERTEST-GFX-NEXT: %[[begin2:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX-NEXT: %[[readfirstlane2:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin2]], i32 %[[mul1]])
// SHADERTEST-GFX-NEXT: %[[sext2:[0-9]+]] = sext i32 %[[readfirstlane2]] to i64
// SHADERTEST-GFX-NEXT: %[[gep3:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext2]]
// SHADERTEST-GFX-NEXT: %[[load3:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep3]], align 4
// SHADERTEST-GFX-NEXT: %[[extract2:[.a-z0-9]+]] = extractelement <8 x i32> %[[load3]], i64 3
// SHADERTEST-GFX-NEXT: %[[and2:[0-9]+]] = and i32 %[[extract2]], 268435455
// SHADERTEST-GFX-NEXT: %[[cmp2:[0-9]+]] = icmp slt i32 %[[extract2]], 0
// SHADERTEST-GFX-NEXT: %[[select2:[0-9]+]] = select i1 %[[cmp2]], i32 %[[extract2]], i32 %[[and2]]
// SHADERTEST-GFX-NEXT: %[[insert2:[.a-z0-9]+]] = insertelement <8 x i32> %[[load3]], i32 %[[select2]], i64 3
// SHADERTEST-GFX-NEXT: %[[shufflevector2:[0-9]+]] = shufflevector <8 x i32> %[[insert2]], <8 x i32> %[[load3]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 12, i32 13, i32 14, i32 15>
// SHADERTEST-GFX-NEXT: %[[gep4:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext2]]
// SHADERTEST-GFX-NEXT: %[[load4:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep4]], align 4
// SHADERTEST-GFX-NEXT: %[[image_call2:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector2]], <4 x i32> %[[load4]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX-NEXT: %[[end2:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin2]], <4 x float> %[[image_call2]])
//
// SHADERTEST-GFX: %[[begin3:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX-NEXT: %[[readfirstlane3:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin3]], i32 %[[mul1]])
// SHADERTEST-GFX-NEXT: %[[sext3:[0-9]+]] = sext i32 %[[readfirstlane3]] to i64
// SHADERTEST-GFX-NEXT: %[[gep5:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext3]]
// SHADERTEST-GFX-NEXT: %[[load5:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep5]], align 4
// SHADERTEST-GFX-NEXT: %[[extract3:[.a-z0-9]+]] = extractelement <8 x i32> %[[load5]], i64 3
// SHADERTEST-GFX-NEXT: %[[and3:[0-9]+]] = and i32 %[[extract3]], 268435455
// SHADERTEST-GFX-NEXT: %[[cmp3:[0-9]+]] = icmp slt i32 %[[extract3]], 0
// SHADERTEST-GFX-NEXT: %[[select3:[0-9]+]] = select i1 %[[cmp3]], i32 %[[extract3]], i32 %[[and3]]
// SHADERTEST-GFX-NEXT: %[[insert3:[.a-z0-9]+]] = insertelement <8 x i32> %[[load5]], i32 %[[select3]], i64 3
// SHADERTEST-GFX-NEXT: %[[shufflevector3:[0-9]+]] = shufflevector <8 x i32> %[[insert3]], <8 x i32> %[[load5]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 12, i32 13, i32 14, i32 15>
// SHADERTEST-GFX-NEXT: %[[gep6:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext3]]
// SHADERTEST-GFX-NEXT: %[[load6:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep6]], align 4
// SHADERTEST-GFX-NEXT: [[image_call3:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector3]], <4 x i32> %[[load6]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX-NEXT: %[[end3:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin3]], <4 x float> %[[image_call3]])
// SHADERTEST-GFX: AMDLLPC SUCCESS
//
// SHADERTEST-GFX_10_3_0-LABEL: {{^// LLPC}} pipeline patching results
// SHADERTEST-GFX_10_3_0: %[[mul1:[0-9]+]] = mul i32 %{{.*}}, 48
// SHADERTEST-GFX_10_3_0-NEXT: %[[begin1:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX_10_3_0-NEXT: %[[readfirstlane1:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin1]], i32 %[[mul1]])
// SHADERTEST-GFX_10_3_0-NEXT: %[[sext1:[0-9]+]] = sext i32 %[[readfirstlane1]] to i64
// SHADERTEST-GFX_10_3_0-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext1]]
// SHADERTEST-GFX_10_3_0-NEXT: %[[load1:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep1]], align 4
// SHADERTEST-GFX_10_3_0-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext1]]
// SHADERTEST-GFX_10_3_0-NEXT: %[[load2:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep2]], align 4
// SHADERTEST-GFX_10_3_0-NEXT: %[[image_call1:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[load1]], <4 x i32> %[[load2]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX_10_3_0-NEXT: %[[end1:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin1]], <4 x float> %[[image_call1]])
//
// SHADERTEST-GFX_10_3_0-NEXT: %[[begin2:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX_10_3_0-NEXT: %[[readfirstlane2:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin2]], i32 %[[mul1]])
// SHADERTEST-GFX_10_3_0-NEXT: %[[sext2:[0-9]+]] = sext i32 %[[readfirstlane2]] to i64
// SHADERTEST-GFX_10_3_0-NEXT: %[[gep3:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext2]]
// SHADERTEST-GFX_10_3_0-NEXT: %[[load3:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep3]], align 4
// SHADERTEST-GFX_10_3_0-NEXT: %[[gep4:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext2]]
// SHADERTEST-GFX_10_3_0-NEXT: %[[load4:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep4]], align 4
// SHADERTEST-GFX_10_3_0-NEXT: %[[image_call2:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[load3]], <4 x i32> %[[load4]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX_10_3_0-NEXT: %[[end2:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin2]], <4 x float> %[[image_call2]])
//
// SHADERTEST-GFX_10_3_0: %[[begin3:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX_10_3_0-NEXT: %[[readfirstlane3:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin3]], i32 %[[mul1]])
// SHADERTEST-GFX_10_3_0-NEXT: %[[sext3:[0-9]+]] = sext i32 %[[readfirstlane3]] to i64
// SHADERTEST-GFX_10_3_0-NEXT: %[[gep5:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext3]]
// SHADERTEST-GFX_10_3_0-NEXT: %[[load5:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep5]], align 4
// SHADERTEST-GFX_10_3_0-NEXT: %[[gep6:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext3]]
// SHADERTEST-GFX_10_3_0-NEXT: %[[load6:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep6]], align 4
// SHADERTEST-GFX_10_3_0-NEXT: [[image_call3:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[load5]], <4 x i32> %[[load6]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX_10_3_0-NEXT: %[[end3:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin3]], <4 x float> %[[image_call3]])
// SHADERTEST-GFX_10_3_0: AMDLLPC SUCCESS
//
// SHADERTEST-GFX_10_3_2-LABEL: {{^// LLPC}} pipeline patching results
// SHADERTEST-GFX_10_3_2: %[[mul1:[0-9]+]] = mul i32 %{{.*}}, 48
// SHADERTEST-GFX_10_3_2-NEXT: %[[begin1:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX_10_3_2-NEXT: %[[readfirstlane1:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin1]], i32 %[[mul1]])
// SHADERTEST-GFX_10_3_2-NEXT: %[[sext1:[0-9]+]] = sext i32 %[[readfirstlane1]] to i64
// SHADERTEST-GFX_10_3_2-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext1]]
// SHADERTEST-GFX_10_3_2-NEXT: %[[load1:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep1]], align 4
// SHADERTEST-GFX_10_3_2-NEXT: %[[extract1:[.a-z0-9]+]] = extractelement <8 x i32> %[[load1]], i64 6
// SHADERTEST-GFX_10_3_2-NEXT: %[[and1:[0-9]+]] = and i32 %[[extract1]], -1048577
// SHADERTEST-GFX_10_3_2-NEXT: %[[insert1:[.a-z0-9]+]] = insertelement <8 x i32> %[[load1]], i32 %[[and1]], i64 6
// SHADERTEST-GFX_10_3_2-NEXT: %[[shufflevector1:[0-9]+]] = shufflevector <8 x i32> %[[insert1]], <8 x i32> %[[load1]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 15>
// SHADERTEST-GFX_10_3_2-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext1]]
// SHADERTEST-GFX_10_3_2-NEXT: %[[load2:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep2]], align 4
// SHADERTEST-GFX_10_3_2-NEXT: %[[image_call1:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector1]], <4 x i32> %[[load2]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX_10_3_2-NEXT: %[[end1:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin1]], <4 x float> %[[image_call1]])
//
// SHADERTEST-GFX_10_3_2-NEXT: %[[begin2:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX_10_3_2-NEXT: %[[readfirstlane2:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin2]], i32 %[[mul1]])
// SHADERTEST-GFX_10_3_2-NEXT: %[[sext2:[0-9]+]] = sext i32 %[[readfirstlane2]] to i64
// SHADERTEST-GFX_10_3_2-NEXT: %[[gep3:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext2]]
// SHADERTEST-GFX_10_3_2-NEXT: %[[load3:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep3]], align 4
// SHADERTEST-GFX_10_3_2-NEXT: %[[extract2:[.a-z0-9]+]] = extractelement <8 x i32> %[[load3]], i64 6
// SHADERTEST-GFX_10_3_2-NEXT: %[[and2:[0-9]+]] = and i32 %[[extract2]], -1048577
// SHADERTEST-GFX_10_3_2-NEXT: %[[insert2:[.a-z0-9]+]] = insertelement <8 x i32> %[[load3]], i32 %[[and2]], i64 6
// SHADERTEST-GFX_10_3_2-NEXT: %[[shufflevector2:[0-9]+]] = shufflevector <8 x i32> %[[insert2]], <8 x i32> %[[load3]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 15>
// SHADERTEST-GFX_10_3_2-NEXT: %[[gep4:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext2]]
// SHADERTEST-GFX_10_3_2-NEXT: %[[load4:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep4]], align 4
// SHADERTEST-GFX_10_3_2-NEXT: %[[image_call2:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector2]], <4 x i32> %[[load4]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX_10_3_2-NEXT: %[[end2:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin2]], <4 x float> %[[image_call2]])
//
// SHADERTEST-GFX_10_3_2: %[[begin3:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul1]])
// SHADERTEST-GFX_10_3_2-NEXT: %[[readfirstlane3:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin3]], i32 %[[mul1]])
// SHADERTEST-GFX_10_3_2-NEXT: %[[sext3:[0-9]+]] = sext i32 %[[readfirstlane3]] to i64
// SHADERTEST-GFX_10_3_2-NEXT: %[[gep5:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext3]]
// SHADERTEST-GFX_10_3_2-NEXT: %[[load5:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep5]], align 4
// SHADERTEST-GFX_10_3_2-NEXT: %[[extract3:[.a-z0-9]+]] = extractelement <8 x i32> %[[load5]], i64 6
// SHADERTEST-GFX_10_3_2-NEXT: %[[and3:[0-9]+]] = and i32 %[[extract3]], -1048577
// SHADERTEST-GFX_10_3_2-NEXT: %[[insert3:[.a-z0-9]+]] = insertelement <8 x i32> %[[load5]], i32 %[[and3]], i64 6
// SHADERTEST-GFX_10_3_2-NEXT: %[[shufflevector3:[0-9]+]] = shufflevector <8 x i32> %[[insert3]], <8 x i32> %[[load5]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 15>
// SHADERTEST-GFX_10_3_2-NEXT: %[[gep6:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext3]]
// SHADERTEST-GFX_10_3_2-NEXT: %[[load6:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep6]], align 4
// SHADERTEST-GFX_10_3_2-NEXT: [[image_call3:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector3]], <4 x i32> %[[load6]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX_10_3_2-NEXT: %[[end3:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin3]], <4 x float> %[[image_call3]])
// SHADERTEST-GFX_10_3_2: AMDLLPC SUCCESS
