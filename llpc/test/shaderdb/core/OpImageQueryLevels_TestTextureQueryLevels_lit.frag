#version 450

layout(set = 0, binding = 0) uniform sampler1D          samp1D;
layout(set = 1, binding = 0) uniform sampler2D          samp2D[4];
layout(set = 0, binding = 1) uniform sampler2DShadow    samp2DShadow;
layout(set = 2, binding = 0) uniform samplerCubeArray   sampCubeArray[4];

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1 = textureQueryLevels(samp1D);
    i1 += textureQueryLevels(samp2D[index]);
    i1 += textureQueryLevels(samp2DShadow);
    i1 += textureQueryLevels(sampCubeArray[index]);

    fragColor = vec4(i1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call i32 (...) @lgc.create.image.query.levels.i32(i32 0, i32 512, <8 x {{.*}})
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call i32 (...) @lgc.create.image.query.levels.i32(i32 1, i32 128, <8 x {{.*}})
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 1, i32 1, i32 0, i32 1)
; SHADERTEST: call i32 (...) @lgc.create.image.query.levels.i32(i32 1, i32 512, <8 x {{.*}})
; SHADERTEST: ptr addrspace(4) (...) @lgc.create.get.desc.ptr.p4{{.*}}(i32 1, i32 1, i32 2, i32 0)
; SHADERTEST: call i32 (...) @lgc.create.image.query.levels.i32(i32 8, i32 128, <8 x {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
