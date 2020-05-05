#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    vec3 f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = radians(f1_1);

    vec3 f3_0 = radians(f3_1);

    fragColor = (f1_0 != f3_0.x) ? vec4(0.5) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <3 x float> %4, <float 0x3F91DF46A0000000, float 0x3F91DF46A0000000, float 0x3F91DF46A0000000>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[0-9]*}} = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: %{{.*}} = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3F91DF46A0000000
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
