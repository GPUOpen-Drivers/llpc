/*
; RUN: not amdllpc -spvgen-dir=%spvgendir% %gfxip %s 2>&1 | FileCheck -check-prefix=SHADERTEST %s
;
; SHADERTEST: Not enough arguments for inline assembly {{.*}} 'my %ra'
*/

#version 450

#extension GL_EXT_debug_printf : enable

layout(location = 0) out vec4 fragColor;

void main()
{
    debugPrintfEXT("my %ra");
    fragColor = vec4(0.0);
}
