#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1, f1_2;
    vec3 f3_1, f3_2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = max(f1_1, f1_2);

    vec3 f3_0 = max(f3_1, f3_2);

    fragColor = (f1_0 != f3_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.maxnum.f32(float %{{.*}}, float %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.maxnum.f32(float %{{.*}}, float %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
