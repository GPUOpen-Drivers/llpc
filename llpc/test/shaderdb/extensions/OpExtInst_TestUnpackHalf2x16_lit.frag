#version 450

layout(binding = 0) uniform Uniforms
{
    uint u1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 f2 = unpackHalf2x16(u1);

    fragColor = (f2.x != f2.y) ? vec4(f2, 0, 0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[BITCAST:.*]] = bitcast i32 %{{.*}} to <2 x half>
; SHADERTEST: = fpext <2 x half> %[[BITCAST]] to <2 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
