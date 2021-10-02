#version 450

layout(constant_id = 1) const int SIZE = 6;

layout(set = 0, binding = 0) uniform UBO
{
    vec4 fa[SIZE];
};

layout(location = 0) in vec4 input0;

void main()
{
    gl_Position = input0 + fa[gl_VertexIndex % fa.length()];
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call i32 (...) @lgc.create.smod.i32(i32
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: srem i32 %{{[^, ]+}}, 6
; SHADERTEST-DAG: icmp slt i32 %{{[^, ]+}}, 0
; SHADERTEST-DAG: icmp ne i32 %{{[^, ]+}}, 0
; SHADERTEST-DAG: and i1 %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST-DAG: add {{.*}}i32{{.*}} 6
; SHADERTEST: select i1 %{{[^, ]+}}, i32 %{{[^, ]+}}, i32 %{{[A-Za-z0-9_.]+}}
; SHADERTEST: getelementptr <{ [6 x [4 x float]] }>,
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
