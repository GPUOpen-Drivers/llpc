#version 450

layout(location = 0) in centroid float f1_1;
layout(location = 1) in flat sample vec4 f4_1;

layout(binding = 0) uniform Uniforms
{
    vec2 offset;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = interpolateAtOffset(f1_1, offset);

    vec4 f4_0 = interpolateAtOffset(f4_1, offset);

    fragColor = (f4_0.y == f1_0) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v -gfxip=11.0.0 %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @interpolateAtOffset.f32.p64.v2f32(ptr addrspace(64) @{{.*}}, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) @{{.*}}, <2 x float> %{{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> @lgc.input.import.builtin.InterpPullMode
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST: fmul reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: fadd reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: fmul reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: fadd reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.input.import.interpolated__f32(
; SHADERTEST-LABEL: _amdgpu_ps_main
; SHADERTEST-COUNT-6: v_fmac_f32_e32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
