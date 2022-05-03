#version 450

layout(binding = 0) uniform Uniforms
{
    int i1_1;
    ivec3 i3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1_0 = sign(i1_1);

    ivec3 i3_0 = sign(i3_1);

    fragColor = ((i1_0 != i3_0.x)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call i32 (...) @lgc.create.ssign.i32(i32
; SHADERTEST: = call <3 x i32> (...) @lgc.create.ssign.v3i32(<3 x i32>
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = icmp {{slt i32 %.*, 1|sgt i32 %.*, 0}}
; SHADERTEST: = select i1 %{{.*}}, {{i32 %.*, i32 1|i32 1, i32 %.*}}
; SHADERTEST: = icmp {{sgt i32 %.*, -1|sge i32 %.*, 0}}
; SHADERTEST: = select i1 %{{.*}}, i32 %{{.*}}, i32 -1
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
