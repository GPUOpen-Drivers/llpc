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
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 512, {{.*}}, i32 0)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
