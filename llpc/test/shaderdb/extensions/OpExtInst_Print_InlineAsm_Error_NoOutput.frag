/*
; RUN: not amdllpc -spvgen-dir=%spvgendir% %gfxip %s 2>&1 | FileCheck -check-prefix=SHADERTEST %s
;
; SHADERTEST: Unexpected string 'v_another_inst' {{.*}} Expected a variable or 'no output'
*/

#version 450

#extension GL_EXT_debug_printf : enable

layout(location = 0) out vec4 fragColor;

void main()
{
    debugPrintfEXT("%ra", "v_inst", "", "v_another_inst");
    fragColor = vec4(0.0);
}
