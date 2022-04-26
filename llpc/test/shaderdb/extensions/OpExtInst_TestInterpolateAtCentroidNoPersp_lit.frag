// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call {{.*}} float @interpolateAtCentroid.p64f32(float addrspace(64)* @{{.*}})
; SHADERTEST: %{{[0-9]*}} = call {{.*}} <4 x float> @interpolateAtCentroid.p64v4f32(<4 x float> addrspace(64)* @{{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: %{{[0-9]*}} = call <2 x float> @lgc.input.import.builtin.InterpLinearCentroid(i32 268435462)
; SHADERTEST: %{{[0-9]*}} = call <2 x float> @lgc.input.import.builtin.InterpPerspCentroid(i32 268435458)
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p1(float %{{.*}}, i32 immarg 0, i32 immarg 0, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p2(float %{{.*}}, float %{{.*}}, i32 immarg 0, i32 immarg 0, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p1(float %{{.*}}, i32 immarg 1, i32 immarg 1, i32 %{{.*}})
; SHADERTEST: %{{[0-9]*}} = call float @llvm.amdgcn.interp.p2(float %{{.*}}, float %{{.*}}, i32 immarg 1, i32 immarg 1, i32 %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450

layout(location = 0) in noperspective centroid float f1_1;
layout(location = 1) in vec4 f4_1;

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = interpolateAtCentroid(f1_1);

    vec4 f4_0 = interpolateAtCentroid(f4_1);

    fragColor = (f4_0.y == f1_0) ? vec4(0.0) : vec4(1.0);
}
