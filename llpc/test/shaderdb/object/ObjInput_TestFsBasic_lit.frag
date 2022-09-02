#version 450

layout(location = 2) in vec4 f4;
layout(location = 5) flat in int i1;
layout(location = 7) flat in uvec2 u2;

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f = f4;
    f += vec4(i1);
    f += vec4(u2, u2);

    fragColor = f;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-DAG: call <2 x i32> (...) @lgc.input.import.interpolated.v2i32{{.*}}
; SHADERTEST-DAG: call i32 (...) @lgc.input.import.interpolated.{{.*}}
; SHADERTEST-DAG: call <4 x float> (...) @lgc.input.import.interpolated.v4f32{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
