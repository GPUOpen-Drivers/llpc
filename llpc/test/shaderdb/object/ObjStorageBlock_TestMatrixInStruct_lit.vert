#version 450

struct PosAttrib
{
    vec4 position;
    int dummy[4];
    vec4 attrib;
};

layout(std140, binding = 0) buffer Buffer
{
    mat4      mvp;
    PosAttrib vertData;
} buf;

void main()
{
    PosAttrib pa = buf.vertData;
    gl_Position = pa.position;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> {{%[^,]+}}, i32 64, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
