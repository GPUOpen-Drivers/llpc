#version 450

struct S1 {
    float a;
    int b;
};

layout(location = 0) flat in S1 in1;
layout(location = 2) flat in S1 in2;
layout(location = 4) flat in int cond;

layout(location = 0) out float outv;

void main()
{
    mat3 m1 = mat3(1.0);
    mat3 m2 = mat3(2.0);
    outv *= (cond < 20 ? m1 : m2)[2][1];

    S1 fv = cond > 5 ? in1 : in2;
    outv *= fv.a;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
