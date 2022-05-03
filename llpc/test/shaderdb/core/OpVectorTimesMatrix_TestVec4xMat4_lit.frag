#version 450 core

layout(location = 0) in vec4 colorIn1;
layout(location = 0) out vec4 color;

struct AA
{
   mat4 bb;
};

layout(binding=2) uniform BB
{

 mat3x4 m2;

};

void main()
{
    vec3 newm =  colorIn1 * m2;
    color.xyz = newm;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.vector.times.matrix.v3f32(<4 x float> %{{.*}}, [3 x <4 x float>] %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
