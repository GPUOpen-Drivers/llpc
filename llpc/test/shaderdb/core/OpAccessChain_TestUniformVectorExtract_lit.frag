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

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: [[GEPVAR:%[^ ]+]] = getelementptr <{ [4 x double], i32 }>, <{ [4 x double], i32 }> addrspace(7)* {{%[^,]+}}, i64 0, i32 0, i64 %
; SHADERTEST: load double, double addrspace(7)* [[GEPVAR]], align 8
; SHADERTEST: [[GEP16:%[^ ]+]] = getelementptr inbounds i8, i8 addrspace(7)* {{%[^,]+}}, i64 16
; SHADERTEST: [[BC:%[^ ]+]] = bitcast i8 addrspace(7)* [[GEP16]] to double addrspace(7)*
; SHADERTEST: load double, double addrspace(7)* [[BC]], align 8
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
