#version 450

layout(set = 0, binding = 0) uniform sampler1D  samp1D;
layout(set = 1, binding = 0) uniform sampler2D  samp2D[4];

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = textureGrad(samp1D, 1.0, 2.0, 3.0);
    f4 += textureGrad(samp2D[index], vec2(4.0), vec2(5.0), vec2(6.0));

    fragColor = f4;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 512, {{.*}}, {{.*}}, i32 25, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 384, {{.*}}, {{.*}}, i32 25, <2 x float> <float 4.000000e+00, float 4.000000e+00>, <2 x float> <float 5.000000e+00, float 5.000000e+00>, <2 x float> <float 6.000000e+00, float 6.000000e+00>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 512, {{.*}}, {{.*}}, i32 25, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.{{p4v8i32|p4}}(i32 1, i32 1, i32 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 384, {{.*}}, {{.*}}, i32 25, <2 x float> <float 4.000000e+00, float 4.000000e+00>, <2 x float> <float 5.000000e+00, float 5.000000e+00>, <2 x float> <float 6.000000e+00, float 6.000000e+00>)

; SHADERTEST-LABEL: pipeline patching results
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.1d.v4f32.f32.f32({{.*}}, float 2.000000e+00, float 3.000000e+00, float 1.000000e+00, {{.*}})
; SHADERTEST: call i32 @llvm.amdgcn.readfirstlane
; SHADERTEST: load <4 x i32>, {{<4 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, {{<8 x i32> addrspace\(4\)\*|ptr addrspace\(4\)}} %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.d.2d.v4f32.f32.f32({{.*}}, float 5.000000e+00, float 5.000000e+00, float 6.000000e+00, float 6.000000e+00, float 4.000000e+00, float 4.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
