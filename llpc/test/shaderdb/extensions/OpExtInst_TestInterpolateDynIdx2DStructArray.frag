#version 450 core

struct Struct_2
{
  float f;
  vec4 v;
};
struct Struct_1
{
    float f;
    Struct_2 s2;
};

layout(location = 0) out vec4 frag_color;
layout(location = 0) in Struct_1 interp[2][3];
layout(location = 20) in flat int x;
layout(location = 21) in flat int y;

void main()
{
    frag_color = interpolateAtOffset(interp[x][y].s2.v, vec2(0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call {{.*}} <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: = call <3 x float> @lgc.input.import.builtin.InterpPullMode
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 2, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 5, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 8, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 11, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 14, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 17, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
