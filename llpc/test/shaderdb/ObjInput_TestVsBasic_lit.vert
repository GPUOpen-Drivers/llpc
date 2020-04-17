#version 450 core

layout(location = 0) in vec4  f4;
layout(location = 1) in int   i1;
layout(location = 2) in uvec2 u2;

void main()
{
    vec4 f = f4;
    f += vec4(i1);
    f += vec4(u2, u2);

    gl_Position = f;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-DAG: call <2 x i32> (...) @lgc.create.read.generic.input.v2i32{{.*}}
; SHADERTEST-DAG: call i32 (...) @lgc.create.read.generic.input.i32{{.*}}
; SHADERTEST-DAG: call <4 x float> (...) @lgc.create.read.generic.input.v4f32{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: define amdgpu_vs void @_amdgpu_vs_main({{.*}}, <4 x float> [[loc0:%[0-9]*]], i32 [[loc1:%[0-9]*]], <2 x i32> [[loc2:%[0-9]*]])
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
