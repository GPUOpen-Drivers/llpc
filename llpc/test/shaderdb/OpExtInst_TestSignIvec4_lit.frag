#version 450 core

layout(location = 0) in vec4 a0;
layout(location = 1) in vec4 b0;
layout(location = 0) out vec4 color;
void main()
{
    ivec4 ua = ivec4(a0);
    color = vec4(sign(ua));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call <4 x i32> (...) @llpc.call.ssign.v4i32(<4 x i32>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: = icmp slt i32 %{{.*}}, 1
; SHADERTEST-DAG: = select i1 %{{.*}}, i32 %{{.*}}, i32 1
; SHADERTEST-DAG: = icmp slt i32 %{{.*}}, 1
; SHADERTEST-DAG: = select i1 %{{.*}}, i32 %{{.*}}, i32 1
; SHADERTEST-DAG: = icmp slt i32 %{{.*}}, 1
; SHADERTEST-DAG: = select i1 %{{.*}}, i32 %{{.*}}, i32 1
; SHADERTEST-DAG: = icmp slt i32 %{{.*}}, 1
; SHADERTEST-DAG: = select i1 %{{.*}}, i32 %{{.*}}, i32 1
; SHADERTEST-DAG: = icmp sgt i32 %{{.*}}, -1
; SHADERTEST-DAG: = select i1 %{{.*}}, i32 %{{.*}}, i32 -1
; SHADERTEST-DAG: = icmp sgt i32 %{{.*}}, -1
; SHADERTEST-DAG: = select i1 %{{.*}}, i32 %{{.*}}, i32 -1
; SHADERTEST-DAG: = icmp sgt i32 %{{.*}}, -1
; SHADERTEST-DAG: = select i1 %{{.*}}, i32 %{{.*}}, i32 -1
; SHADERTEST-DAG: = icmp sgt i32 %{{.*}}, -1
; SHADERTEST: = select i1 %{{.*}}, i32 %{{.*}}, i32 -1
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
