#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;
    dvec3 d3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1;
    double d1_0 = frexp(d1_1, i1);

    ivec3 i3;
    dvec3 d3_0 = frexp(d3_1, i3);

    fragColor = ((d3_0.x != d1_0) || (i3.x != i1)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results

; SHADERTEST: = call reassoc nnan nsz arcp contract double (...) @llpc.call.extract.significand.f64(double
; SHADERTEST: = call i32 (...) @llpc.call.extract.exponent.i32(double
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x double> (...) @llpc.call.extract.significand.v3f64(<3 x double>
; SHADERTEST: = call <3 x i32> (...) @llpc.call.extract.exponent.v3i32(<3 x double>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[0-9]*}} = call double @llvm.amdgcn.frexp.mant.f64(double %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call double @llvm.amdgcn.frexp.mant.f64(double %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call i32 @llvm.amdgcn.frexp.exp.i32.f64(double %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call i32 @llvm.amdgcn.frexp.exp.i32.f64(double %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
