#version 450 core

#define ITER 4

layout(location = 0) out vec4 frag_color;
layout(location = 0) in flat int index;
layout(location = 17) in vec4 interp[ITER];

void main()
{
   frag_color = interpolateAtOffset(interp[index], vec2(0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -verify-ir -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST-COUNT-4: call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.eval.Ij.offset.smooth__v2f32(<2 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
