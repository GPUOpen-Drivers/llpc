#version 450

layout(set = 0, binding = 0) uniform sampler2D samp2D[4];

layout(set = 1, binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 fragColor;

vec4 func(sampler2D s2D, vec2 coord)
{
    return texture(s2D, coord);
}

void main()
{
    vec4 color = func(samp2D[i], vec2(0.5));
    color += func(samp2D[1], vec2(1.0));

    fragColor = color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: call {{.*}} <4 x float> @{{.*}}
; SHADERTEST: define internal {{.*}} <4 x float> @{{.*}}({ { ptr addrspace(4), i32, i32, i32 }, { ptr addrspace(4), i32, i32 } } %{{[a-zA-Z0-9]+}}, ptr addrspace(5) %{{[a-z0-9]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
