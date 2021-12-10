#version 450

layout(binding = 0) uniform Uniforms
{
    dmat2 dm2_0;
    dmat2 dm2_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);


    dmat2 dm2_2 = dm2_0 * dm2_1;

    fragColor = (dm2_2[0] == dm2_0[1]) ? vec4(1.0) : color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [2 x <2 x double>] (...) @lgc.create.matrix.times.matrix.a2v2f64([2 x <2 x double>] %{{[^, ]*}}, [2 x <2 x double>] %{{[^, ]*}})

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: fmul {{.*}}double %{{[^, ]*}}, %{{[^, ]*}}
; SHADERTEST: fadd {{.*}}double %{{[^, ]*}}, %{{[^, ]*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
