#version 450 core

layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput    subIn1;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInputMS  subIn2;
layout(input_attachment_index = 2, set = 0, binding = 2) uniform isubpassInput   subIn3;
layout(input_attachment_index = 3, set = 0, binding = 3) uniform isubpassInputMS subIn4;
layout(input_attachment_index = 4, set = 0, binding = 4) uniform usubpassInput   subIn5;
layout(input_attachment_index = 5, set = 0, binding = 5) uniform usubpassInputMS subIn6;

layout(location = 0) out vec4  fsOut1;
layout(location = 1) out ivec4 fsOut2;
layout(location = 2) out uvec4 fsOut3;

void main()
{
    fsOut1  = subpassLoad(subIn1);
    fsOut1 += subpassLoad(subIn2, 7);
    fsOut2  = subpassLoad(subIn3);
    fsOut2 += subpassLoad(subIn4, 7);
    fsOut3  = subpassLoad(subIn5);
    fsOut3 += subpassLoad(subIn6, 7);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.read.f32.SubpassData{{.*}}({{.*}},{{.*}}, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.fmask.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.read.f32.SubpassData.sample.fmaskbased{{.*}}({{.*}},{{.*}},{{.*}}, i32 7, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 2, i32 0, i1 false)
; SHADERTEST: call <4 x i32> @llpc.image.read.i32.SubpassData{{.*}}({{.*}},{{.*}}, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 3, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.fmask.v8i32(i32 0, i32 3, i32 0, i1 false)
; SHADERTEST: call <4 x i32> @llpc.image.read.i32.SubpassData.sample.fmaskbased{{.*}}({{.*}},{{.*}},{{.*}}, i32 7, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 4, i32 0, i1 false)
; SHADERTEST: call <4 x i32> @llpc.image.read.u32.SubpassData{{.*}}({{.*}},{{.*}}, i32 0,{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.resource.v8i32(i32 0, i32 5, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.desc.load.fmask.v8i32(i32 0, i32 5, i32 0, i1 false)
; SHADERTEST: call <4 x i32> @llpc.image.read.u32.SubpassData.sample.fmaskbased{{.*}}({{.*}},{{.*}},{{.*}}, i32 7, i32 0,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.2d.v4f32.i32(i32 15,{{.*}},{{.*}},{{.*}}, i32 0, i32 0)
; SHADERTEST: call float @llvm.amdgcn.image.load.2d.f32.i32(i32 1,{{.*}},{{.*}},{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32(i32 15,{{.*}},{{.*}},{{.*}},{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
