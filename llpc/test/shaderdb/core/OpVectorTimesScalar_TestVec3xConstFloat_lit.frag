#version 450

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    vec3 f3 = vec3(0.0);
    const float f1 = 1.0;
    f3 *= f1;

    color.xyz = f3;

    fragColor = color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{.*}} = fmul reassoc nnan nsz arcp contract afn <3 x float> %{{.*}}, <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
