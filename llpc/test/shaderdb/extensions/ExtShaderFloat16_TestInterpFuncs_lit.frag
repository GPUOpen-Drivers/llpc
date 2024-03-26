#version 450 core

#extension GL_AMD_gpu_shader_half_float: enable

layout(location = 0) in f16vec4 f16v4;

layout(location = 0) out vec2 fragColor;

void main()
{
    f16vec2 f16v2 = interpolateAtCentroid(f16v4).xy;
    f16v2 += interpolateAtSample(f16v4, 2).xy;
    f16v2 += interpolateAtOffset(f16v4, f16vec2(0.2hf)).xy;

    fragColor = f16v2;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: %{{[A-Za-z0-9]*}} = call <2 x float> @lgc.input.import.builtin.InterpPerspCentroid.v2f32.i32(i32 {{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x half> (...) @lgc.input.import.interpolated__v4f16(i1 false, i32 0, i32 0, i32 0, i32 poison, i32 0, <2 x float> %{{.*}})
; SHADERTEST: = call <2 x float> @lgc.input.import.builtin.SamplePosOffset.v2f32.i32.i32(
; SHADERTEST: = call <3 x float> @lgc.input.import.builtin.InterpPullMode
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
