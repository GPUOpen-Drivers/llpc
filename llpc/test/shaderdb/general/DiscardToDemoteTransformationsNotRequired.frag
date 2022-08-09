// Check that amdllpc does not apply discard-to-demote attribute to legal discards.

// RUN: amdllpc %gfxip --v %s |\
// RUN:   FileCheck %s --check-prefix=CHECK
//
// CHECK-LABEL: {{^}}SPIR-V disassembly
// CHECK:             OpImageSampleImplicitLod
// CHECK:       {{^}} {{OpKill|OpTerminateInvocation}}
// CHECK:             OpImageRead
// CHECK-LABEL: {{^}}// LLPC SPIR-V lowering results
// CHECK:       call void (...) @lgc.create.kill()
// CHECK-LABEL: {{^}}// LLPC pipeline patching results
// CHECK:       call void @llvm.amdgcn.kill(i1 false)
// CHECK-NOT:   "amdgpu-transform-discard-to-demote"
// CHECK-LABEL: {{^}}// LLPC final ELF info
// CHECK:       _amdgpu_ps_main:
// CHECK:       s_wqm_b64 exec, exec
// CHECK-LABEL: {{^}}===== AMDLLPC SUCCESS =====

#version 450

layout (location = 0) in vec2 texCoordIn;
layout (location = 1) in flat int discardPixel;

layout (binding = 0) uniform sampler2D image1;
layout (binding = 1, rgba32f) uniform image2D image2;

layout (location = 0) out vec4 fragColor;

void main() {
  fragColor = texture(image1, texCoordIn);
  if (discardPixel != 0)
    discard;
  fragColor += imageLoad(image2, ivec2(texCoordIn));
}
