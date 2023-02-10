#version 450 core

struct S
{
    vec4 f4;
};

layout(std430, binding = 0) buffer Block
{
    vec3 f3;
    S    s;
} block;

void main()
{
    S s;
    s.f4 = vec4(1.0);

    block.s = s;

    gl_Position = vec4(1.0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: getelementptr { <4 x float> }, ptr addrspace({{.*}}) %{{[a-z0-9]*}}, i32 0, i32 0
; SHADERTEST: getelementptr inbounds (<{ [3 x float], [4 x i8], <{ [4 x float] }> }>, ptr addrspace({{.*}}) @{{.*}}, i32 0, i32 2

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: store <4 x float> <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>,

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
