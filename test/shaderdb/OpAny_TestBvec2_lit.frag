#version 450

layout(binding = 0) uniform Uniforms
{
    bvec2 b2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    if (any(b2) == true)
    {
        color = vec4(1.0);
    }

    fragColor = color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-3: = or i1

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
