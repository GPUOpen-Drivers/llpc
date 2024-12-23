#version 450 core

layout(location = 0) out vec4 frag_color;
layout(location = 0) centroid in vec2 interp;
layout(location = 1) centroid in vec2 interp2[2];
layout(push_constant) uniform PushConstants {
  uint component;
};

void main()
{
    frag_color.x = interpolateAtSample(interp[component], gl_SampleID);
    frag_color.y = interpolateAtSample(interp2[component][0], gl_SampleID);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-COUNT-2: %{{[0-9]*}} = call reassoc nnan nsz arcp contract afn float @interpolateAtSample.f32.p64.i32(ptr addrspace(64) %{{.*}}, i32 %{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.input.import.interpolated__v2f32(i1 false, i32 0
; SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.input.import.interpolated__f32(i1 false, i32 1
: SHADERTEST: call reassoc nnan nsz arcp contract afn float (...) @lgc.input.import.interpolated__f32(i1 false, i32 2
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
