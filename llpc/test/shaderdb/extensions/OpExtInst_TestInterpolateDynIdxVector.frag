#version 450 core

layout(location = 0) out vec4 frag_color;
layout(location = 0) centroid in vec2 interp;
layout(location = 1) centroid in vec2 interp2[2];
layout(push_constant) uniform PushConstants {
  uint component;
};

void main()
{
    frag_color.x = interpolateAtSample(interp[component], gl_SampleID);
    frag_color.y = interpolateAtSample(interp2[component][0], gl_SampleID);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: %{{[0-9]*}} = call {{.*}} float @interpolateAtSample.f32.p64.i32(ptr addrspace(64) %{{.*}}, i32 %{{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST-DAG: = call <2 x float> @lgc.input.import.builtin.SamplePosOffset.v2f32.i32.i32(
; SHADERTEST-DAG: = call <3 x float> @lgc.input.import.builtin.InterpPullMode.v3f32.i32(
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST-DAG: = call <2 x float> @lgc.input.import.interpolant.v2f32.i32.i32.i32.i32.v2f32(i32 0, i32 0, i32 0, i32 0,
; SHADERTEST-DAG: = call <2 x float> @lgc.input.import.builtin.SamplePosOffset.v2f32.i32.i32(
; SHADERTEST-DAG: = call <3 x float> @lgc.input.import.builtin.InterpPullMode.v3f32.i32(
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST-DAG: = call float @lgc.input.import.interpolant.f32.i32.i32.i32.i32.v2f32(i32 1, i32 0, i32 0, i32 0, <2 x float>
; SHADERTEST-DAG: = call <2 x float> @lgc.input.import.builtin.SamplePosOffset.v2f32.i32.i32(
; SHADERTEST-DAG: = call <3 x float> @lgc.input.import.builtin.InterpPullMode.v3f32.i32(
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST-DAG: = call float @lgc.input.import.interpolant.f32.i32.i32.i32.i32.v2f32(i32 2, i32 0, i32 0, i32 0, <2 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
