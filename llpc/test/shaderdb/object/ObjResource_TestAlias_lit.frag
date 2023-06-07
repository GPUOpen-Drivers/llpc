#version 450

layout (std140, binding = 1) uniform BB1
{
    vec4 c1;
};

layout (std140, binding = 1) uniform BB2
{
    vec4 c2;
};

layout (binding = 2) uniform sampler2D s1;
layout (binding = 2) uniform sampler2D s2;

layout (rgba8, binding = 3) uniform image2D i1;
layout (rgba8, binding = 3) uniform image2D i2;

layout (std430, binding = 4) buffer SB1
{
    vec4 sb1;
    vec4 sb2;
};

layout (std430, binding = 4) buffer SB2
{
    vec4 sb_1;
    vec4 sb_2;
};

layout (location = 0) out vec4 frag_color;
void main()
{
    frag_color = c1 + c2;
    sb1 = texture(s1, vec2(0)) + texture(s2, vec2(0));
    sb_2 = imageLoad(i1, ivec2(0)) + imageLoad(i2, ivec2(0));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, <8 x i32>
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, <8 x i32>
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 512, <8 x i32>
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 512, <8 x i32>

; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call {{.*}} {{.*}}@lgc.create.load.buffer.desc.{{[0-9a-z.]*}}{{.*}}(i64 0, i32 1,{{.*}}
; SHADERTEST: load <4 x float>
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.sample.v4f32(i32 1, i32 512, <8 x i32>
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 512, <8 x i32>


; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
