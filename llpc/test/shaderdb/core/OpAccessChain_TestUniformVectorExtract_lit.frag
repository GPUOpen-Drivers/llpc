#version 450

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform Uniforms
{
    dvec4 d4;
    int index;
};

void main()
{
    double d1 = d4[index];
    d1 += d4[2];
    fragColor = vec4(float(d1));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; REQUIRES: do-not-run-me

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: [[GEPVAR:%[^ ]+]] = getelementptr <{ [4 x double], i32 }>, ptr addrspace(7) {{%[^,]+}}, i64 0, i32 0, i64 %
; SHADERTEST: load double, ptr addrspace(7) [[GEPVAR]], align 8
; SHADERTEST: [[GEP16:%[^ ]+]] = getelementptr inbounds <{ [4 x double], i32 }>, ptr addrspace(7) {{%[^,]+}}, i64 0, i32 0, i64 2
; SHADERTEST: load double, ptr addrspace(7) [[GEP16]], align 8
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
