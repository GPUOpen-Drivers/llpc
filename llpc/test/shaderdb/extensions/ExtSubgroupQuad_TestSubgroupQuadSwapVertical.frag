// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.subgroup.quad.swap.vertical.v4f32
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: call i32 @llvm.amdgcn.wqm
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450

#extension GL_KHR_shader_subgroup_quad : require

layout(location = 0) out vec4 result;

layout(set = 0, binding = 4, std430) readonly buffer Buffer0
{
  vec4 data[];
};

void main (void)
{
  result = subgroupQuadSwapVertical(data[gl_SubgroupInvocationID]);
}
