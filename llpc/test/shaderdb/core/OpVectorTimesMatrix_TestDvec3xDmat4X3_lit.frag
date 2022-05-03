#version 450

layout(binding = 0) uniform Uniforms
{
    dvec3 d3;
    dmat4x3 dm4x3;
    dvec4 d4_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    dvec4 d4_0 = d3 * dm4x3;

    fragColor = (d4_0 != d4_1) ? vec4(1.0) : color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract <4 x double> (...) @lgc.create.vector.times.matrix.v4f64(<3 x double> %{{.*}}, [4 x <3 x double>] %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
