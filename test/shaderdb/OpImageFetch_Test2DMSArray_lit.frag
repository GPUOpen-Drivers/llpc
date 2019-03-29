#version 450 core

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
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.fmask.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <4 x float> @llpc.image.fetch.f32.2DArray.sample.fmaskbased{{.*}}({{.*}},{{.*}},{{.*}}, i32 2,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call float @llvm.amdgcn.image.load.3d.f32.i32(i32 1,{{.*}},{{.*}},{{.*}},{{.*}}, i32 0, i32 0)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.load.2darraymsaa.v4f32.i32(i32 15,{{.*}},{{.*}},{{.*}},{{.*}},{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
