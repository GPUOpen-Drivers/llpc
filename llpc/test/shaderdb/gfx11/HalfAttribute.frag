// Check that f16 attribute was interpolated using rtz intrinsic.

// RUN: amdllpc %gfxip --v %s |\
// RUN:   FileCheck %s --check-prefix=CHECK
//
// CHECK-LABEL: {{^}}// LLPC pipeline patching results
// CHECK:  [[P:%.*]] = call float @llvm.amdgcn.lds.param.load(i32 0, i32 0, i32 %PrimMask)
// CHECK:  [[P1:%.*]] = call float @llvm.amdgcn.interp.p10.rtz.f16(float [[P]], float %PerspInterpCenter.i0, float [[P]], i1 false)
// CHECK:  [[P2:%.*]]  = call half @llvm.amdgcn.interp.p2.rtz.f16(float [[P]], float %PerspInterpCenter.i1, float [[P1]], i1 false)
// CHECK-LABEL: {{^}}===== AMDLLPC SUCCESS =====


#version 450
#extension GL_AMD_gpu_shader_half_float: enable

layout (location = 0) in f16vec2 texCoordIn;
layout (binding = 0) uniform sampler2D image1;
layout (location = 0) out vec4 fragColor;

void main() {
  fragColor = texture(image1, texCoordIn);
}
