#version 450 core

layout(location = 0) in vec4 a;
layout(location = 0) out vec4 frag_color;

void main()
{
    float fv = tanh(a.x);
    frag_color = vec4(fv);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.tanh.f32(float
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[^ ]+}} = fmul reassoc nnan nsz arcp contract afn float %{{[^, ]+}}, 0x3FF7154760000000
; SHADERTEST: %{{[^ ]+}} = {{fsub|fneg}} reassoc nnan nsz arcp contract afn float {{(-0.000000e+00, )?}}%{{[A-Za-z0-9_.]+}}
; SHADERTEST: %{{[^ ]+}} = call reassoc nnan nsz arcp contract afn float @llvm.exp2.f32(float %{{[^) ]+}})
; SHADERTEST: %{{[^ ]+}} = call reassoc nnan nsz arcp contract afn float @llvm.exp2.f32(float %{{[^) ]+}})
; SHADERTEST: %{{[^ ]+}} = fsub reassoc nnan nsz arcp contract afn float %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: %{{[^ ]+}} = fadd reassoc nnan nsz arcp contract afn float %{{[^, ]+}}, %{{[A-Za-z0-9_.]+}}
; SHADERTEST: %{{[^ ]+}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fdiv.fast(float %{{[^, ]+}}, float %{{[^) ]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
