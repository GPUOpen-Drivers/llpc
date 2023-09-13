#version 450 core

layout(set = 0, binding = 0) uniform sampler2DShadow samp;
layout(location = 0) in vec3 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = vec4(textureLodOffset(samp, inUV, 1, ivec2(2, 3)));
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 2, i32 2, i64 0, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.create.image.sample.f32(i32 1, i32 512, <8 x i32>{{.*}}, i32 801,{{.*}}, float 1.000000e+00, <2 x i32> <i32 2, i32 3>,

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} float @llvm.amdgcn.image.sample.c.l.o.2d.f32.f32(i32 1, i32 770,{{.*}},{{.*}},{{.*}}, float 1.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
