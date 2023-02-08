#version 450 core

layout(set = 0, binding = 0) uniform sampler2DMS samp;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    ivec2 iUV = ivec2(inUV);
    oColor = texelFetch(samp, iUV, 2);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 5, i32 5, i32 0, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.with.fmask.v4f32(i32 6, i32 1536, {{.*}}, i32 2)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2dmsaa.v4f32.i32(i32 15,{{.*}},{{.*}},{{.*}},{{.*}}, i32 0, i32 0), !invariant.load
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
