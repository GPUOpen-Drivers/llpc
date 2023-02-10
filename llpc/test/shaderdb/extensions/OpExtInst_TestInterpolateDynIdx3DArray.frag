#version 450 core

layout(location = 0) out vec4 frag_color;
layout(location = 0) in flat int x;
layout(location = 1) in flat int y;
layout(location = 2) in flat int z;
layout(location = 3) in vec4 interp[2][3][2];

void main()
{
    frag_color = interpolateAtOffset(interp[x][y][z], vec2(0));
}



// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST: %{{[0-9]*}} = call {{.*}} <4 x float> @interpolateAtOffset.v4f32.p64.v2f32(ptr addrspace(64) %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: = call <3 x float> @lgc.input.import.builtin.InterpPullMode
; SHADERTEST-COUNT-12: = call i32 @llvm.amdgcn.mov.dpp.i32(i32
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 3, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 4, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 5, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 6, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 7, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 8, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 9, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 10, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 11, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 12, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 13, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @lgc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 14, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
