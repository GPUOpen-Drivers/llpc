#version 450

layout(set = 0, binding = 0) uniform sampler2DRect       samp2DRect;
layout(set = 1, binding = 0) uniform samplerBuffer       samp2DBuffer[4];
layout(set = 0, binding = 1) uniform sampler2DMS         samp2DMS;
layout(set = 2, binding = 0) uniform sampler2DMSArray    samp2DMSArray[4];

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out ivec3 i3;

void main()
{
    i3 = ivec3(0);
    i3.xy += textureSize(samp2DRect);
    i3.x  += textureSize(samp2DBuffer[index]);
    i3.xy += textureSize(samp2DMS);
    i3    += textureSize(samp2DMSArray[index]);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.i32(i32 0, i32 128, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 6, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 7, i32 128, {{.*}}, i32 0)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
