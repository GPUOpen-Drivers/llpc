#version 450

layout(set = 1, binding = 0) uniform BB
{
   vec4 m1;
   vec4 m2[10];
} b[4];


layout(location = 0) out vec4 o1;

void main()
{
    o1 = b[0].m1 + b[3].m2[5];
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: getelementptr inbounds ([4 x <{ [4 x float], [10 x [4 x float]] }>], ptr addrspace({{.*}}) @{{.*}}, i32 0, i32 3, i32 1, i32 5

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} {{.*}}@lgc.create.load.buffer.desc.{{[0-9a-z.]*}}(i64 1, i32 0, i32 0
; SHADERTEST: call {{.*}} {{.*}}@lgc.create.load.buffer.desc.{{[0-9a-z.]*}}(i64 1, i32 0, i32 3

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.s.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 96, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
