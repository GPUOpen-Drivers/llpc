#version 450

layout(set = 0, binding = 0) uniform sampler2DMS          samp2DMS;
layout(set = 0, binding = 1) uniform sampler2DMSArray     samp2DMSArray[4];

layout(set = 1, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1 = textureSamples(samp2DMS);
    i1 += textureSamples(samp2DMSArray[index]);

    fragColor = vec4(i1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.samples.i32(i32 6, i32 512, {{.*}})
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.image.query.samples.i32(i32 7, i32 128, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
