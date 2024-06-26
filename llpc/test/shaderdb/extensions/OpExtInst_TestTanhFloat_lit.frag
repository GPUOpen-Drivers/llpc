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
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.tanh.f32(float
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.fabs.f32(float %{{[0-9]*}})
; SHADERTEST: %{{[0-9]*}} = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0xC007154760000000
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.exp2.f32(float %{{.*}})
; SHADERTEST: %{{[0-9]*}} = fadd reassoc nnan nsz arcp contract afn float %{{.*}}, 1.000000e+00
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fdiv.fast(float 2.000000e+00, float %{{.*}})
; SHADERTEST: %{{[0-9]*}} = fsub reassoc nnan nsz arcp contract afn float 1.000000e+00, %{{.*}}
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @llvm.copysign.f32(float %{{.*}}, float %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
