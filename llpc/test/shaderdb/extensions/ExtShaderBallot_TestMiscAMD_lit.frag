#version 450 core

#extension GL_ARB_gpu_shader_int64: enable
#extension GL_ARB_shader_ballot: enable
#extension GL_AMD_shader_ballot: enable

layout(location = 0) out vec4 fv4Out;

layout(location = 0) in flat ivec2 iv2In;
layout(location = 1) in flat uvec3 uv3In;
layout(location = 2) in flat vec4  fv4In;

void main()
{
    vec4 fv4 = vec4(0.0);

    uint64_t u64 = ballotARB(true);
    uint u = mbcntAMD(u64);
    fv4.x += u;

    fv4.xy  += writeInvocationAMD(iv2In, ivec2(1, 2), 0);
    fv4.xyz += writeInvocationAMD(uv3In, uvec3(3, 4, 5), 1);
    fv4     += writeInvocationAMD(fv4In, vec4(6.0, 7.0, 8.0, 9.0), 2);

    fv4Out = fv4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call <4 x i32> (...) @lgc.create.subgroup.ballot.v4i32(
; SHADERTEST: call i32 (...) @lgc.create.subgroup.mbcnt.i32(
; SHADERTEST: call <2 x i32> (...) @lgc.create.subgroup.write.invocation.v2i32(
; SHADERTEST: call <3 x i32> (...) @lgc.create.subgroup.write.invocation.v3i32(
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.subgroup.write.invocation.v4f32(
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
