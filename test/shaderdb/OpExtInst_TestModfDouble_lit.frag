#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;
    dvec3 d3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    double d1_0, d1_2;
    d1_0 = modf(d1_1, d1_2);

    dvec3 d3_0, d3_2;
    d3_0 = modf(d3_1, d3_2);

    fragColor = ((d1_0 != d3_0.x) || (d1_2 == d3_2.y)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract double @llvm.trunc.f64(double %{{.*}})
; SHADERTEST: %{{[0-9]*}} = fsub reassoc nnan nsz arcp contract double %{{.*}}, %{{.*}}
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract <3 x double> @llvm.trunc.v3f64(<3 x double> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = fsub reassoc nnan nsz arcp contract <3 x double> %{{.*}}, %{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
