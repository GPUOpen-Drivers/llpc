#version 450

layout(set = 0, binding = 0) uniform sampler2D samp2D_0[4];
layout(set = 1, binding = 0) uniform sampler2D samp2D_1[4];

layout(set = 2, binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 fragColor;

vec4 func0(sampler2D s2D, vec2 coord)
{
    return texture(s2D, coord);
}

vec4 func1(sampler2D s2D[4], vec2 coord)
{
    return func0(s2D[2], coord) + func0(s2D[i], coord);
}

void main()
{
    vec4 color = func1(samp2D_0, vec2(1.0));
    color *= func1(samp2D_1, vec2(0.5));

    fragColor = color;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: call {{.*}} <4 x float> @{{.*}}
; SHADERTEST: define internal {{.*}}<4 x float> @{{.*}}({ { {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}}, i32, i32, i32 }, { {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}}, i32, i32 } } %{{[a-zA-Z0-9]+}}, {{<2 x float> addrspace\(5\)\*|ptr addrspace\(5\)}} %{{[a-z0-9]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
