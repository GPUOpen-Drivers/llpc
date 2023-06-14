#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    int i1;

    vec3 f3_1;
    ivec3 i3;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = ldexp(f1_1, i1);

    vec3 f3_0 = ldexp(f3_1, i3);

    fragColor = ((f3_0.x != f1_0) || (i3.x != i1)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.ldexp.f32(float %{{.*}}, i32
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.ldexp.v3f32(<3 x float> %{{.*}}, <3 x i32>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.ldexp.f32.i32(float %{{.*}}, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.ldexp.f32.i32(float %{{.*}}, i32 %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
