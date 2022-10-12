#version 450

layout(set = 0, binding = 0) uniform sampler2D samp2D_0;
layout(set = 0, binding = 1) uniform sampler2D samp2D_1;

layout(location = 0) out vec4 fragColor;

vec4 func(sampler2D s2D, vec2 coord)
{
    return texture(s2D, coord);
}

void main()
{
    fragColor  = func(samp2D_0, vec2(1.0));
    fragColor += func(samp2D_1, vec2(0.0));
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: call {{.*}} <4 x float> @{{.*}}
; SHADERTEST: define internal {{.*}}<4 x float> @{{.*}}({ { {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}}, i32 }, { {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}}, i32, i32 } } %{{[a-zA-Z0-9]+}}, {{<2 x float> addrspace\(5\)\*|ptr addrspace\(5\)}} %{{[a-z0-9]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
