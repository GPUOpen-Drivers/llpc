#version 450

layout(binding = 0) uniform Uniforms
{
    uint u1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 f2 = unpackUnorm2x16(u1);

    fragColor = (f2.x != f2.y) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[BITCAST:.*]] = bitcast i32 %1 to <2 x i16>
; SHADERTEST: %[[CONV:.*]] = uitofp <2 x i16> %[[BITCAST]] to <2 x float>
; SHADERTEST: = fmul reassoc nnan nsz arcp contract <2 x float> %[[CONV]], <float 0x3EF0001000000000, float 0x3EF0001000000000>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
