/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call float asm sideeffect "v_mul_f32 $0, $1, 2.0", "=v,r"(float
; SHADERTEST: AMDLLPC SUCCESS
*/

#version 450

#extension GL_EXT_debug_printf : enable

layout(binding = 0) uniform Uniforms
{
    float x;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float x = x;

    debugPrintfEXT("%ra", "v_mul_f32 $0, $1, 2.0", "=v,r", x, x);

    fragColor = vec4(x);
}
