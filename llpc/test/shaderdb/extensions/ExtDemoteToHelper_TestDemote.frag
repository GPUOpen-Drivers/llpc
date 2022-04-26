// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call void (...) @lgc.create.demote.to.helper.invocation()
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.wqm.demote(i1 false)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450

#extension GL_EXT_demote_to_helper_invocation: enable

layout (location = 0) in vec2 texCoordIn;
layout (location = 1) in flat int discardPixel;

layout (binding = 0) uniform sampler2D image;

layout (location = 0) out vec4 fragColor;

void main()
{
    if  (discardPixel != 0) {
    	demote;      
    }
    fragColor = texture(image, texCoordIn);
}
