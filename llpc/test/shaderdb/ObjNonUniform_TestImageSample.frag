#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=0,binding=5) uniform sampler samp;
layout(set=0,binding=6) uniform texture2D data[];
layout(location = 0) out vec4     FragColor;
layout(location = 0) in flat int  index1;
layout(location = 1) in flat int  index2;

void main()
{
  vec4 color1 = texture(nonuniformEXT(sampler2D(data[index1], samp)), vec2(0,0));
  vec4 color2 = texture(sampler2D(data[index2], samp), vec2(1,1));
  FragColor = color1 + color2;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: {{%[0-9]*}} = call float @llvm.amdgcn.interp.mov
; SHADERTEST: {{%[0-9]*}} = bitcast float {{%[0-9]*}} to i32
; SHADERTEST: {{%[0-9]*}} = call i32 @llvm.amdgcn.readfirstlane(i32 {{%[0-9]*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST