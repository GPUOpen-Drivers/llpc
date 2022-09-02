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
; RUN: amdllpc -auto-layout-desc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call <2 x float> @lgc.input.import.generic.v2f32(i1 false, i32 0, i32 0, i32 1, i32 poison)
; SHADERTEST: call float @lgc.input.import.generic.f32(i1 false, i32 0, i32 0, i32 0, i32 poison)
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call {{.*}} @llvm.amdgcn.struct.tbuffer.load
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
