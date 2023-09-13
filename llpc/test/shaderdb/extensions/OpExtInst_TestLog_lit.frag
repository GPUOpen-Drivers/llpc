#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    vec3 f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = log(f1_1);

    vec3 f3_0 = log(f3_1);

    fragColor = ((f1_0 != 0.0) && (f3_0.x < 0.1)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.log.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.log.v3f32(<3 x float>
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.log.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.log.v3f32(<3 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float @llvm.log2.f32(float
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0x3FE62E4300000000
; SHADERTEST-NOT: = call{{.*}} @llvm.log2.f32(float
; SHADERTEST-LABEL: {{^// LLPC}} final pipeline module info
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
