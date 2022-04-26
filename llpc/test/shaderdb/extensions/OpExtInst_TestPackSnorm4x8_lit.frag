#version 450

layout(binding = 0) uniform Uniforms
{
    vec4 f4;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    uint u1 = packSnorm4x8(f4);

    fragColor = (u1 != 5) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[CLAMP:.*]] = call <4 x float> (...) @lgc.create.fclamp.v4f32(<4 x float> %{{.*}}, <4 x float> <float -1.000000e+00, float -1.000000e+00, float -1.000000e+00, float -1.000000e+00>, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>)
; SHADERTEST: %[[SCALE:.*]] = fmul <4 x float> %[[CLAMP]], <float 1.270000e+02, float 1.270000e+02, float 1.270000e+02, float 1.270000e+02>
; SHADERTEST: %[[RINT:.*]] = call <4 x float> @llvm.rint.v4f32(<4 x float> %[[SCALE]])
; SHADERTEST: %[[CONV:.*]] = fptosi <4 x float> %[[RINT]] to <4 x i8>
; SHADERTEST: = bitcast <4 x i8> %[[CONV]] to i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
