#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;
    int i1;

    dvec3 d3_1;
    ivec3 i3;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    double d1_0 = ldexp(d1_1, i1);

    dvec3 d3_0 = ldexp(d3_1, i3);

    fragColor = ((d3_0.x != d1_0) || (i3.x != i1)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract double (...) @lgc.create.ldexp.f64(double
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x double> (...) @lgc.create.ldexp.v3f64(<3 x double>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract double @llvm.ldexp.f64.i32(double %{{.*}}, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract double @llvm.ldexp.f64.i32(double %{{.*}}, i32 %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
