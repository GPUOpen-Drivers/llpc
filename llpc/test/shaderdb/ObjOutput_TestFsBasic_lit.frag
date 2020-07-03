#version 450 core

layout(location = 0) in flat int i0;
layout(location = 1) in float i1;

layout(location = 0) out int o0;
layout(location = 1) out float o1;

void main()
{
    o0 = i0;
    o1 = i1;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call void @lgc.output.export.generic{{.*}}i32
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f32
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: [[value1:%[0-9]*]] = insertvalue { float, float } undef, float {{%[0-9]*}}, 0
; SHADERTEST: [[value2:%[0-9]*]] = insertvalue { float, float } [[value1]], float {{%[0-9]*}}, 1
; SHADERTEST: ret { float, float } [[value2]]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
