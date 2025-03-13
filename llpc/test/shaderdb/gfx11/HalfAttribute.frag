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

// Check that f16 attribute was interpolated using rtz intrinsic.

// RUN: amdllpc %gfxip --v %s |\
// RUN:   FileCheck %s --check-prefix=CHECK
//
// CHECK-LABEL: {{^}}// LLPC LGC lowering results
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
