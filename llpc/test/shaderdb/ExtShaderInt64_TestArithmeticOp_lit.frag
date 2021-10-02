#version 450

#extension GL_ARB_gpu_shader_int64 : enable

layout(std140, binding = 0) uniform Uniforms
{
    int64_t  i64;
    u64vec3  u64v3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    u64vec3 u64v3_0 = u64vec3(0);
    u64v3_0 += u64v3;
    u64v3_0 -= u64v3;
    u64v3_0 *= u64v3;
    u64v3_0 /= u64v3;
    u64v3_0 %= u64v3;

    int64_t i64_0 = 0;
    i64_0 += i64;
    i64_0 -= i64;
    i64_0 *= i64;
    i64_0 /= i64;
    i64_0 %= i64;

    u64v3_0.x = -i64;
    fragColor = vec3(u64v3_0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: add <3 x i64> %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: sub <3 x i64> %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: mul <3 x i64> %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: udiv <3 x i64> %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: urem <3 x i64> %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: add i64 %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: sub i64 %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: mul i64 %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: sdiv i64 %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: call i64 (...) @lgc.create.smod.i64(i64
; SHADERTEST: sub nsw i64 0, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
