#version 450

layout(binding = 0) uniform Uniforms
{
    vec2 f2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    uint u1 = packSnorm2x16(f2);

    fragColor = (u1 != 5) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[CLAMP:.*]] = call <2 x float> (...) @lgc.create.fclamp.v2f32(<2 x float> %{{.*}}, <2 x float> <float -1.000000e+00, float -1.000000e+00>, <2 x float> <float 1.000000e+00, float 1.000000e+00>)
; SHADERTEST: %[[SCALE:.*]] = fmul <2 x float> %[[CLAMP]], <float 3.276700e+04, float 3.276700e+04>
; SHADERTEST: %[[RINT:.*]] = call <2 x float> @llvm.rint.v2f32(<2 x float> %[[SCALE]])
; SHADERTEST: %[[CONV:.*]] = fptosi <2 x float> %[[RINT]] to <2 x i16>
; SHADERTEST: = bitcast <2 x i16> %[[CONV]] to i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
