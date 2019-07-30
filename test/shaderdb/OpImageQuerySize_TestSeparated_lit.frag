#version 450 core
layout(set = 0, binding = 0) uniform sampler samp;
layout(binding = 1) uniform texture2D  image;
layout(location = 0) out vec4 oColor;

void main()
{
    ivec2 s = textureSize(sampler2D(image, samp), 0);
    oColor = vec4(s.x, s.y, 0, 1);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 1) 
; SHADERTEST: call {{.*}} @llpc.call.image.query.size.v2i32(i32 1, i32 0, {{.*}}, i32 0) 

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getresinfo.2d.v2f32.i32(i32 3, i32 0,{{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
