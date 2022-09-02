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
; RUN: amdllpc -auto-layout-desc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-DAG: call <2 x i32> @lgc.input.import.generic.v2i32{{.*}}
; SHADERTEST-DAG: call i32 @lgc.input.import.generic{{.*}}
; SHADERTEST-DAG: call <4 x float> @lgc.input.import.generic.v4f32{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST-COUNT-3: call {{.*}} @llvm.amdgcn.struct.tbuffer.load
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
