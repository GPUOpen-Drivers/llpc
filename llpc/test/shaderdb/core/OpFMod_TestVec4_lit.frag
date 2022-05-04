#version 450

layout(binding = 0) uniform Uniforms
{
    vec4 f4_1;
    float f1;
    float f2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4_0 = vec4(f2);
    f4_0 = mod(f4_0, f4_1);

    f4_0 += mod(f4_0, f1);

    fragColor = (f4_0.y > 0.0) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.fmod.v4f32(<4 x float>
; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: fdiv reassoc nnan nsz arcp contract afn float
; SHADERTEST: call reassoc nnan nsz arcp contract afn float @llvm.floor.f32
; SHADERTEST: fsub reassoc nnan nsz arcp contract afn float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
