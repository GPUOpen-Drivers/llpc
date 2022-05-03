#version 450

layout(binding = 0) uniform Uniforms
{
    vec3 f3_0, f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    bvec3 b3 = lessThanEqual(f3_0, f3_1);

    fragColor = (b3.x) ? vec4(1.0) : vec4(0.5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: load <3 x float>,
; SHADERTEST: fcmp ole float
; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: fcmp ole float
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
