#version 450

layout(set = 0, binding = 0, rgba32f) uniform image1D           img1D;
layout(set = 0, binding = 1, rgba32f) uniform image2DRect       img2DRect;
layout(set = 0, binding = 2, rgba32f) uniform image2DMS         img2DMS;
layout(set = 1, binding = 0, rgba32f) uniform imageBuffer       imgBuffer[4];
layout(set = 2, binding = 0, rgba32f) uniform imageCubeArray    imgCubeArray[4];

layout(set = 3, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    ivec3 i3 = ivec3(0);
    i3.x  += imageSize(img1D);
    i3.xy += imageSize(img2DRect);
    i3.xy += imageSize(img2DMS);
    i3.x  += imageSize(imgBuffer[index]);
    i3    += imageSize(imgCubeArray[index + 1]);

    fragColor = (i3.x != 5) ? vec4(1.0) : vec4(0.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 2, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.i32(i32 0, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 9, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 6, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.i32(i32 10, i32 128, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 8, i32 128, {{.*}}, i32 0)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
