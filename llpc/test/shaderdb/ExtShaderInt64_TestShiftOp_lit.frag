#version 450

#extension GL_ARB_gpu_shader_int64 : enable

layout(std430, binding = 0) buffer Uniforms
{
    int64_t  i64;
    uint64_t u64;
    int      i1;
    uint     u1;

    i64vec3  i64v3;
    u64vec3  u64v3;
};

layout(location = 0) out float fragColor;

void main()
{
    u64vec3 u64v3_0 = u64v3 << i64;
    i64vec3 i64v3_0 = i64v3 << u64;

    u64v3_0 >>= i1;
    i64v3_0 >>= u1;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: shl <3 x i64> %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: lshr <3 x i64> %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: ashr <3 x i64> %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
