#version 450 core

#define ITER 5

struct Struct_2
{
   float f1;
   vec4 array[ITER];
};
struct Struct_1
{
   float f1;
   Struct_2 s2;
};

layout(location = 0) out vec4 frag_color;
layout(location = 0) in flat int index;
layout(location = 1) in Struct_1 interp;

void main()
{
    frag_color = interpolateAtOffset(interp.s2.array[index], vec2(0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call {{.*}} <4 x float> @_Z19interpolateAtOffsetPDv4_fDv2_f(<4 x float> addrspace(64)* %{{.*}}, <2 x float> {{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: %{{[0-9]*}} = call <2 x float> @llpc.input.interpolate.evalij.offset.v2f32(<2 x float> {{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @llpc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 3, i32 0, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <2 x float> @llpc.input.interpolate.evalij.offset.v2f32(<2 x float> {{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @llpc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 3, i32 1, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <2 x float> @llpc.input.interpolate.evalij.offset.v2f32(<2 x float> {{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @llpc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 3, i32 2, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <2 x float> @llpc.input.interpolate.evalij.offset.v2f32(<2 x float> {{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @llpc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 3, i32 3, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call <2 x float> @llpc.input.interpolate.evalij.offset.v2f32(<2 x float> {{.*}})
; SHADERTEST: %{{[0-9]*}} = call <4 x float> @llpc.input.import.interpolant.v4f32.i32.i32.i32.i32.v2f32(i32 3, i32 4, i32 0, i32 0, <2 x float> %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

