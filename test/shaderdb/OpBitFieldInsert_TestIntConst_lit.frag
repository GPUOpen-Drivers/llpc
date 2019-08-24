#version 450 core

layout(location = 0) in vec4 colorIn1;
layout(location = 1) in vec4 colorIn2;
layout(location = 0) out vec4 color;
void main()
{
    ivec4 bd = ivec4(colorIn1);
    color = vec4(bitfieldInsert(3, 6, 4, 5));
    // output 0x42c60000
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call i32 (...) @llpc.call.insert.bit.field.i32(i32 3, i32 6, i32 4, i32 5)

; SHADERTEST-LABEL: {{^// LLPC.*}} pipeline patching results
; SHADERTEST: call void @llvm.amdgcn.exp.f32(i32 immarg 0, i32 immarg 15, float 9.900000e+01, float 9.900000e+01, float 9.900000e+01, float 9.900000e+01, i1 immarg true, i1 immarg true)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
