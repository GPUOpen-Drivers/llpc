// Check that flat parameter load uses DPP in strict WQM

// RUN: amdllpc %gfxip --v %s |\
// RUN:   FileCheck %s --check-prefix=CHECK
//
// CHECK-LABEL: {{^}}// LLPC pipeline patching results
// CHECK:       call void @llvm.amdgcn.kill(i1 false)
// CHECK:       [[P0:%.*]] = call float @llvm.amdgcn.lds.param.load(i32 immarg 2, i32 immarg 2, i32 %PrimMask)
// CHECK:       [[P1:%.*]] = bitcast float [[P0]] to i32
// CHECK:       [[P2:%.*]] = call i32 @llvm.amdgcn.mov.dpp.i32(i32 [[P1]], i32 0, i32 15, i32 15, i1 true)
// CHECK:       [[P3:%.*]] = call i32 @llvm.amdgcn.strict.wqm.i32(i32 [[P2]])
// CHECK-LABEL: {{^}}===== AMDLLPC SUCCESS =====

#version 450

layout (location = 0) in vec3 texCoordIn;
layout (location = 1) in float discardPixel;
layout (location = 2) flat in vec4 p0;

layout (binding = 0) uniform sampler2D image1;
layout (binding = 1) uniform sampler s0;
layout (binding = 2) uniform textureCube t0;

layout (location = 0) out vec4 fragColor;

void main() {
  fragColor = texture(image1, texCoordIn.xy);

  if (discardPixel > 0.0)
    discard;

  float lod = p0.z;
  fragColor += textureLod(samplerCube(t0, s0), texCoordIn, lod);
}
