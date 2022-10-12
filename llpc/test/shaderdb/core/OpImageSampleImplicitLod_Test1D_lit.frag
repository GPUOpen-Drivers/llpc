#version 450 core

layout(set = 0, binding = 0) uniform sampler1D samp;
layout(location = 0) in float inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = texture(samp, inUV);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 512, {{.*}}, {{.*}}, i32 1, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 512, {{.*}}, {{.*}}, i32 1, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.1d.v4f32.f32

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
