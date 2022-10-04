#version 450 core

layout(set = 0, binding = 0) uniform sampler2DShadow samp;
layout(location = 0) in vec3 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = vec4(textureGradOffset(samp, inUV, vec2(0.1, 0.2), vec2(0.3, 0.4), ivec2(2, 3)));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 793, {{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FC99999A0000000>, <2 x float> <float 0x3FD3333340000000, float 0x3FD99999A0000000>, <2 x i32> <i32 2, i32 3>, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.f32(i32 1, i32 512, {{.*}}, {{.*}}, i32 793, {{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FC99999A0000000>, <2 x float> <float 0x3FD3333340000000, float 0x3FD99999A0000000>, <2 x i32> <i32 2, i32 3>, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} float @llvm.amdgcn.image.sample.c.d.o.2d.f32.f32.f32({{.*}}, i32 770, {{.*}}, float 0x3FB99999A0000000, float 0x3FC99999A0000000, float 0x3FD3333340000000, float 0x3FD99999A0000000, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
