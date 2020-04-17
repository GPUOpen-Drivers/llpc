#version 450 core

layout(location = 0, component = 0) in float f1;
layout(location = 0, component = 1) in vec2  f2;

layout(location = 0) out vec3 color;

void main()
{
    vec3 f3 = vec3(f1, f2);
    color = f3;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call reassoc nnan nsz arcp contract <2 x float> (...) @lgc.create.read.generic.input.v2f32(i32 0, i32 0, i32 1, i32 0, i32 0, i32 undef)
; SHADERTEST: call reassoc nnan nsz arcp contract float (...) @lgc.create.read.generic.input.f32(i32 0, i32 0, i32 0, i32 0, i32 0, i32 undef)
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: define amdgpu_vs void @_amdgpu_vs_main({{.*}}, float [[comp0:%[0-9]*]], <2 x float> [[comp1:%[0-9]*]])
; SHADERTEST: [[comp1x:%[0-9]*]] = extractelement <2 x float> [[comp1]], i32 0
; SHADERTEST: [[comp1y:%[0-9]*]] = extractelement <2 x float> [[comp1]], i32 1
; SHADERTEST: call void @llvm.amdgcn.exp.f32(i32 immarg 32, i32 immarg 7, float [[comp0]], float [[comp1x]], float [[comp1y]], float undef, i1 immarg false, i1 immarg false)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
