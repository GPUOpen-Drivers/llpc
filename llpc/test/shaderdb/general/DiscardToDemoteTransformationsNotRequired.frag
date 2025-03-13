/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/

// Check that amdllpc does not apply discard-to-demote attribute to legal discards.

// RUN: amdllpc %gfxip --v %s |\
// RUN:   FileCheck %s --check-prefix=CHECK
//
// CHECK-LABEL: {{^}}SPIR-V disassembly
// CHECK:             OpImageSampleImplicitLod
// CHECK:       {{^}} {{OpKill|OpTerminateInvocation}}
// CHECK:             OpImageRead
// CHECK-LABEL: {{^}}// LLPC FE lowering results
// CHECK:       call void (...) @lgc.create.kill()
// CHECK-LABEL: {{^}}// LLPC LGC lowering results
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
