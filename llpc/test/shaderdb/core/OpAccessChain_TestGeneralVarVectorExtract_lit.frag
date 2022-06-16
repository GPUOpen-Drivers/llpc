#version 450

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform Uniforms
{
    int index;
};

void main()
{
    dvec3 d3 = dvec3(0.0);
    d3[index] = 0.5;
    d3[2] = 1.0;

    vec3 f3 = vec3(0.0);
    f3[index] = 2.0;

    double d1 = d3[1];
    float  f1 = f3[index] + f3[1];

    fragColor = vec4(float(d1), f1, f1, f1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s


; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: getelementptr <3 x double>, <3 x double> addrspace({{.*}})* %{{.*}}, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: getelementptr <3 x float>, <3 x float> addrspace({{.*}})* %{{.*}}, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: getelementptr <3 x float>, <3 x float> addrspace({{.*}})* %{{.*}}, i32 0, i32 %{{[0-9]*}}

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: getelementptr <3 x double>, <3 x double> addrspace({{.*}})* %{{.*}}, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: load double, double addrspace({{.*}})* %{{.*}}

; SHADERTEST: getelementptr <3 x float>, <3 x float> addrspace({{.*}})* %{{.*}}, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: load float, float addrspace({{.*}})* %{{.*}}

; SHADERTEST: getelementptr <3 x float>, <3 x float> addrspace({{.*}})* %{{.*}}, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: load float, float addrspace({{.*}})* %{{.*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
