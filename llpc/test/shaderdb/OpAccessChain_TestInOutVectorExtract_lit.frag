#version 450

layout(location = 0) flat in vec4 color[4];
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform Uniforms
{
    int index;
};

void main()
{
    float f1 = color[index][index + 1];
    fragColor[index] = f1;
    fragColor[1] = 0.4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: add i32 %{{[^, ]+}}, 1
; SHADERTEST: getelementptr [4 x <4 x float>], [4 x <4 x float>] addrspace({{.*}})* %{{[^, ]+}}, i32 0, i32 %{{[^, ]+}}, i32 %{{[A-Za-z0-9_.]+}}
; SHADERTEST: load i32, i32 addrspace({{.*}})* %{{[A-Za-z0-9_.]+}}

; SHADERTEST: icmp eq i32 %{{[^, ]+}}, 1
; SHADERTEST: select i1 %{{[^, ]+}}, float addrspace({{.*}})* %{{[^, ]+}}, float addrspace({{.*}})* %{{[A-Za-z0-9_.]+}}
; SHADERTEST: icmp eq i32 %{{[^, ]+}}, 2
; SHADERTEST: select i1 %{{[^, ]+}}, float addrspace({{.*}})* %{{[^, ]+}}, float addrspace({{.*}})* %{{[A-Za-z0-9_.]+}}
; SHADERTEST: icmp eq i32 %{{[^, ]+}}, 3
; SHADERTEST: select i1 %{{[^, ]+}}, float addrspace({{.*}})* %{{[^, ]+}}, float addrspace({{.*}})* %{{[A-Za-z0-9_.]+}}
; SHADERTEST: store float %{{[^, ]+}}, float addrspace({{.*}})* %{{[A-Za-z0-9_.]+}}
; SHADERTEST: store float 0x3FD99999A0000000, float addrspace({{.*}})* %{{[A-Za-z0-9_.]+}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
