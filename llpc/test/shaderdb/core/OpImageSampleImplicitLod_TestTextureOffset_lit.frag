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
    vec4 f4 = textureOffset(samp1D, 1.0, 2, 0.4);
    f4 += textureOffset(samp2D[index], vec2(0.5), ivec2(5));

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 512, {{.*}}, {{.*}}, i32 321, float 1.000000e+00, float 0x3FD99999A0000000, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 384, {{.*}}, {{.*}}, i32 257, <2 x float> <float 5.000000e-01, float 5.000000e-01>, <2 x i32> <i32 5, i32 5>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 0, i32 512, {{.*}}, {{.*}}, i32 321, float 1.000000e+00, float 0x3FD99999A0000000, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 1, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 384, {{.*}}, {{.*}}, i32 257, <2 x float> <float 5.000000e-01, float 5.000000e-01>, <2 x i32> <i32 5, i32 5>)

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.b.o.1d.v4f32.f32.f32({{.*}}, i32 2, float 0x3FD99999A0000000, float 1.000000e+00, {{.*}})
; SHADERTEST: call i32 @llvm.amdgcn.readfirstlane
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.o.2d.v4f32.f32({{.*}}, i32 1285, float 5.000000e-01, float 5.000000e-01, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
