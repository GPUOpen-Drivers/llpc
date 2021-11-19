#version 450

layout(binding = 0) uniform Uniforms
{
    uint u1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = unpackSnorm4x8(u1);

    fragColor = (f4.x != f4.y) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[BITCAST:.*]] = bitcast i32 %{{[0-9]+}} to <4 x i8>
; SHADERTEST: %[[CONV:.*]] = sitofp <4 x i8> %[[BITCAST]] to <4 x float>
; SHADERTEST: %[[SCALE:.*]] = fmul reassoc nnan nsz arcp contract afn <4 x float> %[[CONV]], <float 0x3F80204080000000, float 0x3F80204080000000, float 0x3F80204080000000, float 0x3F80204080000000>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.fclamp.v4f32(<4 x float> %[[SCALE]], <4 x float> <float -1.000000e+00, float -1.000000e+00, float -1.000000e+00, float -1.000000e+00>, <4 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
