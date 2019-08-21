#version 450 core

#extension GL_ARB_shader_ballot: enable
#extension GL_ARB_gpu_shader_int64: enable

layout(location = 0) in flat int i1;
layout(location = 1) in float f1;

layout(location = 0) out float f;

void main(void)
{
    uint64_t u64 = 0;

    u64 += gl_SubGroupInvocationARB;
    u64 += gl_SubGroupSizeARB;
    u64 += gl_SubGroupEqMaskARB;
    u64 += gl_SubGroupGeMaskARB;
    u64 += gl_SubGroupGtMaskARB;
    u64 += gl_SubGroupLeMaskARB;
    u64 += gl_SubGroupLtMaskARB;

    u64 += ballotARB(true);

    f = float(u64);

    f += float(readInvocationARB(i1, 1));
    f += float(readFirstInvocationARB(i1));

    f += readInvocationARB(f1, 2);
    f += readFirstInvocationARB(f1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call <4 x i32> (...) @llpc.call.subgroup.ballot.v4i32(
; SHADERTEST: call i32 (...) @llpc.call.subgroup.broadcast.i32(
; SHADERTEST: call i32 (...) @llpc.call.subgroup.broadcast.first.i32(
; SHADERTEST: call float (...) @llpc.call.subgroup.broadcast.f32(
; SHADERTEST: call float (...) @llpc.call.subgroup.broadcast.first.f32(
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: call <4 x i32> @llpc.input.import.builtin.SubgroupLtMask.v4i32.i32
; SHADERTEST: call <4 x i32> @llpc.input.import.builtin.SubgroupLeMask.v4i32.i32
; SHADERTEST: call <4 x i32> @llpc.input.import.builtin.SubgroupGtMask.v4i32.i32
; SHADERTEST: call <4 x i32> @llpc.input.import.builtin.SubgroupGeMask.v4i32.i32
; SHADERTEST: call <4 x i32> @llpc.input.import.builtin.SubgroupEqMask.v4i32.i32
; SHADERTEST: call i32 @llpc.input.import.builtin.SubgroupSize.i32.i32
; SHADERTEST: call i32 @llpc.input.import.builtin.SubgroupLocalInvocationId.i32.i32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
