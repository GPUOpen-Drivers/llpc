#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 7) uniform sampler2D _11[];

layout(location = 0) out vec4 _3;
layout(location = 3) flat in int _4;
layout(location = 1) flat in vec2 _6;

void main()
{
    int _12 = _4;
    _3 = texture(_11[nonuniformEXT(_12)], _6);
}

// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v %gfxip %s | FileCheck -check-prefix=SHADERTEST-GFX %s
// Explicitly check GFX10.3 ASIC variants:
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v --gfxip=10.3.0 %s | FileCheck -check-prefix=SHADERTEST-GFX_10_3_0 %s
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v --gfxip=10.3.2 %s | FileCheck -check-prefix=SHADERTEST-GFX_10_3_2 %s
// Make sure that both the image resource desc and sample desc have the same index and there is only one
// waterfall.readfirstlane for both of them

// SHADERTEST-GFX-LABEL: {{^// LLPC}} pipeline patching results
// SHADERTEST-GFX: %[[mul:[0-9]+]] = mul i32 %{{.*}}, 48
// SHADERTEST-GFX-NEXT: %[[begin:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul]])
// SHADERTEST-GFX-NEXT: %[[readfirstlane:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin]], i32 %[[mul]])
// SHADERTEST-GFX-NEXT: %[[sext:[0-9]+]] = sext i32 %[[readfirstlane]] to i64
// SHADERTEST-GFX-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// SHADERTEST-GFX-NEXT: %[[load1:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep1]], align 4
// SHADERTEST-GFX-NEXT: %[[extract:[.a-z0-9]+]] = extractelement <8 x i32> %[[load1]], i64 3
// SHADERTEST-GFX-NEXT: %[[and:[0-9]+]] = and i32 %[[extract]], 268435455
// SHADERTEST-GFX-NEXT: %[[cmp:[0-9]+]] = icmp slt i32 %[[extract]], 0
// SHADERTEST-GFX-NEXT: %[[select:[0-9]+]] = select i1 %[[cmp]], i32 %[[extract]], i32 %[[and]]
// SHADERTEST-GFX-NEXT: %[[insert:[.a-z0-9]+]] = insertelement <8 x i32> %[[load1]], i32 %[[select]], i64 3
// SHADERTEST-GFX-NEXT: %[[shufflevector:[0-9]+]] = shufflevector <8 x i32> %[[insert]], <8 x i32> %[[load1]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 12, i32 13, i32 14, i32 15>
// SHADERTEST-GFX-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// SHADERTEST-GFX-NEXT: %[[load2:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep2]], align 4
// SHADERTEST-GFX-NEXT: %[[image_call:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector]], <4 x i32> %[[load2]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX-NEXT: %[[end:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin]], <4 x float> %[[image_call]])
// SHADERTEST-GFX: AMDLLPC SUCCESS
//
// SHADERTEST-GFX_10_3_0-LABEL: {{^// LLPC}} pipeline patching results
// SHADERTEST-GFX_10_3_0: %[[mul:[0-9]+]] = mul i32 %{{.*}}, 48
// SHADERTEST-GFX_10_3_0-NEXT: %[[begin:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul]])
// SHADERTEST-GFX_10_3_0-NEXT: %[[readfirstlane:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin]], i32 %[[mul]])
// SHADERTEST-GFX_10_3_0-NEXT: %[[sext:[0-9]+]] = sext i32 %[[readfirstlane]] to i64
// SHADERTEST-GFX_10_3_0-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// SHADERTEST-GFX_10_3_0-NEXT: %[[load1:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep1]], align 4
// SHADERTEST-GFX_10_3_0-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// SHADERTEST-GFX_10_3_0-NEXT: %[[load2:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep2]], align 4
// SHADERTEST-GFX_10_3_0-NEXT: %[[image_call:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[load1]], <4 x i32> %[[load2]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX_10_3_0-NEXT: %[[end:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin]], <4 x float> %[[image_call]])
// SHADERTEST-GFX_10_3_0: AMDLLPC SUCCESS
//
// SHADERTEST-GFX_10_3_2-LABEL: {{^// LLPC}} pipeline patching results
// SHADERTEST-GFX_10_3_2: %[[mul:[0-9]+]] = mul i32 %{{.*}}, 48
// SHADERTEST-GFX_10_3_2-NEXT: %[[begin:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul]])
// SHADERTEST-GFX_10_3_2-NEXT: %[[readfirstlane:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin]], i32 %[[mul]])
// SHADERTEST-GFX_10_3_2-NEXT: %[[sext:[0-9]+]] = sext i32 %[[readfirstlane]] to i64
// SHADERTEST-GFX_10_3_2-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// SHADERTEST-GFX_10_3_2-NEXT: %[[load1:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep1]], align 4
// SHADERTEST-GFX_10_3_2-NEXT: %[[extract:[.a-z0-9]+]] = extractelement <8 x i32> %[[load1]], i64 6
// SHADERTEST-GFX_10_3_2-NEXT: %[[and:[0-9]+]] = and i32 %[[extract]], -1048577
// SHADERTEST-GFX_10_3_2-NEXT: %[[insert:[.a-z0-9]+]] = insertelement <8 x i32> %[[load1]], i32 %[[and]], i64 6
// SHADERTEST-GFX_10_3_2-NEXT: %[[shufflevector:[0-9]+]] = shufflevector <8 x i32> %[[insert]], <8 x i32> %[[load1]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 15>
// SHADERTEST-GFX_10_3_2-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// SHADERTEST-GFX_10_3_2-NEXT: %[[load2:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep2]], align 4
// SHADERTEST-GFX_10_3_2-NEXT: %[[image_call:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector]], <4 x i32> %[[load2]], i1 false, i32 0, i32 0)
// SHADERTEST-GFX_10_3_2-NEXT: %[[end:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin]], <4 x float> %[[image_call]])
// SHADERTEST-GFX_10_3_2: AMDLLPC SUCCESS
