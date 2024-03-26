#version 450 core

layout(triangles) in;

struct S
{
    int     x;
    vec4    y;
    float   z[2];
};

layout(location = 2) in TheBlock
{
    S     blockS;
    float blockFa[3];
    S     blockSa[2];
    float blockF;
} teBlock[];

layout(location = 0) out float f;

layout(binding = 0) uniform Uniforms
{
    int i;
};

void main(void)
{
    S block = teBlock[2].blockSa[1];

    f = teBlock[1].blockSa[i].z[i + 1];
    f += block.z[1] + block.y[0];
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-DAG: call float @lgc.input.import.generic__f32{{.*}}
; SHADERTEST-DAG: call float @lgc.input.import.generic{{.*}}
; SHADERTEST-DAG: call <4 x float> @lgc.input.import.generic__v4f32{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
