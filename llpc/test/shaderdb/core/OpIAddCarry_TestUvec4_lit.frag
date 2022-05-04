#version 450 core

layout(location = 0) in vec4 colorIn1;
layout(location = 0) out vec4 color;
void main()
{
    uvec4 bd = uvec4(colorIn1);
    uint outi = 0;
    uvec4 outv = uvec4(0);
    uint out1 = uaddCarry(bd.x,bd.y,outi);
    uvec4 out0 = uaddCarry(bd,bd,outv);
    color = vec4(out0 + outv);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32
; SHADERTEST: = call { <4 x i32>, <4 x i1> } @llvm.uadd.with.overflow.v4i32(<4 x i32>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: call { <4 x i32>, <4 x i1> } @llvm.uadd.with.overflow.v4i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
