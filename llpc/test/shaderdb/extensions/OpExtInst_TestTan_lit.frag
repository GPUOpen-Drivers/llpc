#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    vec3 f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = tan(f1_1);

    vec3 f3_0 = tan(f3_1);

    fragColor = (f1_0 != f3_0.x) ? vec4(0.5) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.tan.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.tan.v3f32(<3 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.sin.v3f32(<3 x float>
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.cos.v3f32(<3 x float>
; SHADERTEST: %{{[0-9]*}} = fdiv reassoc nnan nsz arcp contract afn <3 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>,
; SHADERTEST: %{{[0-9]*}} = fmul reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: %{{[0-9]*}} = fcmp {{[ou]}}ne float %{{.*}}, %{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
