#version 450 core

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
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 2, i32 2, i32 0, i32 0
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 516, <8 x {{.*}}, <4 x {{.*}}, i32 37, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 516, <8 x {{.*}}, <4 x {{.*}}, i32 293, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, <2 x i32> <i32 1, i32 2>)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 516, <8 x {{.*}}, <4 x {{.*}}, i32 293, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, [4 x <2 x i32>] [<2 x i32> <i32 1, i32 1>, <2 x i32> <i32 2, i32 2>, <2 x i32> <i32 3, i32 3>, <2 x i32> <i32 4, i32 4>])
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 1
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 2, i32 2, i32 0, i32 1
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 512, <8 x {{.*}}, <4 x {{.*}}, i32 37, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 512, <8 x {{.*}}, <4 x {{.*}}, i32 293, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, <2 x i32> <i32 1, i32 2>)
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.gather.v4i32(i32 1, i32 512, <8 x {{.*}}, <4 x {{.*}}, i32 293, <2 x float> <float 0.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, [4 x <2 x i32>] [<2 x i32> <i32 1, i32 1>, <2 x i32> <i32 2, i32 2>, <2 x i32> <i32 3, i32 3>, <2 x i32> <i32 4, i32 4>])

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getresinfo.2d.v2f32.i32(i32 3, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.2d.v4f32.f32(i32 1,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 513,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 257,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 514,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 771,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 1028,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getresinfo.2d.v2f32.i32(i32 3, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getresinfo.2d.v2f32.i32(i32 3, i32 0, <8 x i32> %{{[-0-9A-Za0z_.]+}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.2d.v4f32.f32(i32 1,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 513,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 257,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 514,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 771,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.gather4.lz.o.2d.v4f32.f32(i32 1, i32 1028,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
