#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    vec3 f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = round(f1_1);

    vec3 f3_0 = round(f3_1);

    fragColor = ((f1_0 != f3_0.x)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.rint.f32(float %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.rint.v3f32(<3 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
