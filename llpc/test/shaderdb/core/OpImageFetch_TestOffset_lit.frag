#version 450 core

layout(set = 0, binding = 0) uniform sampler2D samp;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    ivec2 iUV = ivec2(inUV);
    oColor = texelFetchOffset(samp, iUV, 2, ivec2(3, 4));
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// GLSL}} program compile/link log
; SHADERTEST: {{.*}} OpImageFetch {{.*}} Lod|ConstOffset {{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.v4f32(i32 1, i32 1536, {{.*}}, i32 2)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.mip.2d.v4f32.i32(i32 15,{{.*}},{{.*}}, i32 2,{{.*}}, i32 0, i32 0), !invariant.load
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
