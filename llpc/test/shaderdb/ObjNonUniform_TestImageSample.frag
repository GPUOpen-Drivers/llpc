#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=0,binding=0) uniform sampler2D samp2Ds[];
layout(set=0,binding=1) uniform sampler2D samp2D;
layout(set=0,binding=2) uniform sampler samp;
layout(set=0,binding=3) uniform texture2D image;
layout(set=0,binding=4) uniform sampler samps[];
layout(set=0,binding=5) uniform texture2D images[];

layout(location = 0) out vec4     FragColor;
layout(location = 0) in flat int  index1;
layout(location = 1) in flat int  index2;

void main()
{
  vec4 color1 = vec4(0);
  color1 += texture(samp2D, vec2(0, 0));
  color1 += texture(sampler2D(image, samp), vec2(0,0));
  color1 += texture(samp2Ds[0], vec2(0,0));
  color1 += texture(sampler2D(images[index1], samps[index2]), vec2(0, 0));

  color1 += texture(nonuniformEXT(sampler2D(images[index1], samp)), vec2(0,0));

  FragColor = color1;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 0
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 0
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 0
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 384
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 24
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST-COUNT-12: call i32 @llvm.amdgcn.readfirstlane
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: {{%[0-9]*}} = call float @llvm.amdgcn.interp.mov
; SHADERTEST: {{%[0-9]*}} = bitcast float {{%[0-9]*}} to i32
; SHADERTEST: {{%[0-9]*}} = call i32 @llvm.amdgcn.readfirstlane(i32 {{%[0-9]*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST