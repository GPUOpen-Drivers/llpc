#version 450 core

layout(set = 0, binding = 0) uniform sampler2DShadow samp2DS;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    const ivec2 offsets[4] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
    oColor = textureGatherOffsets(samp2DS, inUV, 1.0, offsets);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 2, i32 2, i64 0, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.gather.v4f32(i32 1, i32 512, ptr addrspace(4) {{.*}}, ptr addrspace(4) {{.*}}, i32 801, <2 x {{.*}}, float 0.000000e+00, [4 x <2 x i32>] [<2 x i32> <i32 1, i32 2>, <2 x i32> <i32 3, i32 4>, <2 x i32> <i32 5, i32 6>, <2 x i32> <i32 7, i32 8>], float 1.000000e+00)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32(i32 1, i32 513, float 1.000000e+00,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32(i32 1, i32 1027, float 1.000000e+00,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32(i32 1, i32 1541, float 1.000000e+00,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.c.lz.o.2d.v4f32.f32(i32 1, i32 2055, float 1.000000e+00,{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
