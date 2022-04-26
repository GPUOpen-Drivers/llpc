#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    vec3 f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = fract(f1_1);

    vec3 f3_0 = fract(f3_1);

    fragColor = ((f1_0 != f3_0.x)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.fract.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.fract.v3f32(<3 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fract.f32(float %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fract.f32(float %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
