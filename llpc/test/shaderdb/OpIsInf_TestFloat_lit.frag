#version 450

layout(binding = 0) uniform Uniforms
{
    float f1;
};

layout(location = 0) out vec4 f;

void main()
{
    f = (isinf(f1)) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call i1 (...) @lgc.create.isinf.i1(float

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i1 @llvm.amdgcn.class.f32(float %{{[^, ]+}}, i32 516)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
