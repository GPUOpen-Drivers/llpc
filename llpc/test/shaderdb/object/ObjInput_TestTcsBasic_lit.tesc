#version 450 core

layout(vertices = 3) out;

layout(location = 1) in float inData1[];
layout(location = 2) in dvec4 inData2[];

void main (void)
{
    gl_out[gl_InvocationID].gl_Position = vec4(inData1[gl_InvocationID]);
    gl_out[gl_InvocationID].gl_PointSize = float(inData2[gl_InvocationID].z);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.generic__f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call double @lgc.input.import.generic__f64{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
