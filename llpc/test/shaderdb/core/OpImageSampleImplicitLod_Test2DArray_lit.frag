#version 450 core

layout(set = 0, binding = 0) uniform sampler2DArray samp;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = texture(samp, vec3(1, 2, 3));
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 1, <3 x float> <float 1.000000e+00, float 2.000000e+00, float 3.000000e+00>)

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 5, i32 512, {{.*}}, {{.*}}, i32 1, <3 x float> <float 1.000000e+00, float 2.000000e+00, float 3.000000e+00>)

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: load <4 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, ptr addrspace(4) %{{[0-9]*}}
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.sample.2darray.v4f32.f32({{.*}}, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
