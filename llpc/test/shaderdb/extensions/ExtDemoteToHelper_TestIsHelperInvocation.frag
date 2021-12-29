// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call i1 (...) @lgc.create.is.helper.invocation.i1()
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i1 @llvm.amdgcn.live.mask()
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450

#extension GL_EXT_demote_to_helper_invocation: enable

layout (location = 0) in flat int discardPixel;

layout (location = 0) out vec4 fragColor;

void main()
{
    if  (discardPixel != 0) {
    	demote;      
    }
    bool isHelper = helperInvocationEXT();
    fragColor = isHelper ? vec4(1.0f) : vec4(0.0f);
}
