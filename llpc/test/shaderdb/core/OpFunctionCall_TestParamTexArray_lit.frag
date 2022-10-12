#version 450

layout(set = 0, binding = 0) uniform sampler2D samp2D_0[4];
layout(set = 1, binding = 0) uniform sampler2D samp2D_1[4];

layout(set = 2, binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 fragColor;

vec4 func(sampler2D s2D[4], vec2 coord)
{
    vec4 color = texture(s2D[2], coord);
    color += texture(s2D[i], coord);

    return color;
}

void main()
{
    fragColor = func(samp2D_0, vec2(0.5)) + func(samp2D_1, vec2(1.0));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: call {{.*}} <4 x float> @{{.*}}
; SHADERTEST: define internal {{.*}}<4 x float> @{{.*}}({ { ptr addrspace(4), i32 }, { ptr addrspace(4), i32, i32 } } %{{[a-zA-Z0-9]+}}, ptr addrspace(5) %{{[a-z0-9]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
