/*
; RUN: not amdllpc -spvgen-dir=%spvgendir% %gfxip %s 2>&1 | FileCheck -check-prefix=SHADERTEST %s
;
; SHADERTEST: Unexpected value '' {{.*}} Expected a variable
*/

#version 450

#extension GL_EXT_debug_printf : enable

layout(location = 0) out vec4 fragColor;

void main()
{
    debugPrintfEXT("%ra", "v_mov $0, 1.0", "", 1);
    fragColor = vec4(0.0);
}
