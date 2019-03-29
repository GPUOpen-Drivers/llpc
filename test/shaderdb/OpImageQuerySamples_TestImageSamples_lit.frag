#version 450

layout(set = 0, binding = 0, rgba32f) uniform image2DMS         img2DMS;
layout(set = 0, binding = 1, rgba32f) uniform image2DMSArray    img2DMSArray[4];

layout(set = 1, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    int i1 = imageSamples(img2DMS);
    i1 += imageSamples(img2DMSArray[index]);

    fragColor = vec4(i1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.samples{{.*}}({{.*}},{{.*}})
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 1,{{.*}}, i1 false)
; SHADERTEST: call i32 @llpc.image.querynonlod.samples{{.*}}({{.*}},{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
