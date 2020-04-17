#version 450 core

layout(location = 2) in dvec4 d4;
layout(location = 4) in dvec3 d3[2];
layout(location = 8) in dmat4 dm4;

layout(binding = 0) uniform Uniforms
{
    int i;
};

void main()
{
    dvec4 d = d4;
    d += dvec4(d3[i], 1.0);
    d += dm4[i];

    gl_Position = vec4(d);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-DAG: call <4 x double> (...) @lgc.create.read.generic.input.v4f64(i32 8, i32 0,{{.*}}
; SHADERTEST-DAG: call <4 x double> (...) @lgc.create.read.generic.input.v4f64(i32 8, i32 2,{{.*}}
; SHADERTEST-DAG: call <4 x double> (...) @lgc.create.read.generic.input.v4f64(i32 8, i32 4,{{.*}}
; SHADERTEST-DAG: call <4 x double> (...) @lgc.create.read.generic.input.v4f64(i32 8, i32 6,{{.*}}
; SHADERTEST-DAG: call <3 x double> (...) @lgc.create.read.generic.input.v3f64(i32 4, i32 0,{{.*}}
; SHADERTEST-DAG: call <3 x double> (...) @lgc.create.read.generic.input.v3f64(i32 4, i32 2,{{.*}}
; SHADERTEST-DAG: call <4 x double> (...) @lgc.create.read.generic.input.v4f64(i32 2, i32 0,{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: define amdgpu_vs void @_amdgpu_vs_main({{.*}}, <4 x double> [[loc2:%[0-9]*]], <3 x double> [[loc4_0:%[0-9]*]], <3 x double> [[loc4_1:%[0-9]*]], <4 x double> [[loc8_0:%[0-9]*]], <4 x double> [[loc8_1:%[0-9]*]], <4 x double> [[loc8_2:%[0-9]*]], <4 x double> [[loc8_3:%[0-9]*]])
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
