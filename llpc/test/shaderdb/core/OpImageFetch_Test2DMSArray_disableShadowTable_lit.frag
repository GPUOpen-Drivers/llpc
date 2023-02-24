// Shadow table is disabled, F-mask doesn't need to load descriptor, append the provided sample number to coordinates.

// BEGIN_SHADERTEST
// RUN: amdllpc -v %gfxip --enable-shadow-desc=false %s | FileCheck -check-prefix=SHADERTEST %s
// SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
// SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0
// SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 5, i32 5, i32 0, i32 0
// SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.image.load.with.fmask.v4f32(i32 7, i32 1536, {{.*}}, i32 2)

// SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
// "i32 2" is provided sample number
// SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.load.2darraymsaa.v4f32.i32(i32 15, {{.*}}, {{.*}}, {{.*}}, i32 2, <8 x i32> %{{[0-9]*}}, i32 0, i32 0)
// SHADERTEST: AMDLLPC SUCCESS
// END_SHADERTEST

#version 450 core

layout(set = 0, binding = 0) uniform sampler2DMSArray samp;
layout(location = 0) in vec3 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    ivec3 iUV = ivec3(inUV);
    oColor = texelFetch(samp, iUV, 2);
}
