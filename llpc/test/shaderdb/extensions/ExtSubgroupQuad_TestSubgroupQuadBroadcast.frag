// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.subgroup.quad.broadcast.f32
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.subgroup.quad.broadcast.f32
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.subgroup.quad.broadcast.f32
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.subgroup.quad.broadcast.f32
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450

#extension GL_KHR_shader_subgroup_quad : require

layout(binding = 0) readonly buffer Block0
{
    float alpha[];
};

layout(location = 0) out vec4 color;

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    float v = alpha[coord.y * 2 + coord.x];

    vec4 lanes;
    lanes.x = subgroupQuadBroadcast(v, 0u);
    lanes.y = subgroupQuadBroadcast(v, 1u);
    lanes.z = subgroupQuadBroadcast(v, 2u);
    lanes.w = subgroupQuadBroadcast(v, 3u);

    color = lanes;
}
