#version 450

layout(set = 0, binding = 0) uniform sampler2D            samp2D[4];
layout(set = 0, binding = 4) uniform sampler2DShadow      samp2DShadow;
layout(set = 0, binding = 5) uniform sampler3D            samp3D;

layout(set = 1, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 2) out ivec3 i3;

void main()
{
    i3 = ivec3(0);
    i3.xy += textureSize(samp2D[index], 3);
    i3.xy += textureSize(samp2DShadow, 4);
    i3    += textureSize(samp3D, 5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 128, {{.*}}, i32 3)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 4)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 512, {{.*}}, i32 4)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 5)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 2, i32 512, {{.*}}, i32 5)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
