#version 450 core

#extension GL_AMD_shader_explicit_vertex_parameter: enable

layout(location = 0) out vec2 fOut;

void main()
{
    fOut = gl_BaryCoordNoPerspAMD;
    fOut += gl_BaryCoordNoPerspCentroidAMD;
    fOut += gl_BaryCoordNoPerspSampleAMD;
    fOut += gl_BaryCoordSmoothAMD;
    fOut += gl_BaryCoordSmoothCentroidAMD;
    fOut += gl_BaryCoordSmoothSampleAMD;
    fOut += gl_BaryCoordPullModelAMD.xy + gl_BaryCoordPullModelAMD.yz;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-DAG: call <3 x float> @llpc.input.import.builtin.BaryCoordPullModel.v3f32.i32
; SHADERTEST-DAG: call <2 x float> @llpc.input.import.builtin.BaryCoordSmoothSample.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @llpc.input.import.builtin.BaryCoordSmoothCentroid.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @llpc.input.import.builtin.BaryCoordSmooth.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @llpc.input.import.builtin.BaryCoordNoPerspSample.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @llpc.input.import.builtin.BaryCoordNoPerspCentroid.v2f32.i32
; SHADERTEST-DAG: call <2 x float> @llpc.input.import.builtin.BaryCoordNoPersp.v2f32.i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
