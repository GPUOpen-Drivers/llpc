#version 450

layout(binding = 0) uniform Uniforms
{
    vec4 f4;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    uint u1 = packUnorm4x8(f4);

    fragColor = (u1 != 5) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[CLAMP:.*]] = call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.fclamp.v4f32(<4 x float> %{{.*}}, <4 x float> zeroinitializer, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>)
; SHADERTEST: %[[SCALE:.*]] = fmul reassoc nnan nsz arcp contract <4 x float> %[[CLAMP]], <float 2.550000e+02, float 2.550000e+02, float 2.550000e+02, float 2.550000e+02>
; SHADERTEST: %[[CONV:.*]] = fptoui <4 x float> %[[SCALE]] to <4 x i8>
; SHADERTEST: = bitcast <4 x i8> %[[CONV]] to i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
