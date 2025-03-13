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

// This test is to verify that imageAtomicAdd with float type is correctly translated to the HW instruction
// image_atomic_add_flt on GFX12.

// RUN: amdllpc -v -gfxip=12.0.1 %s | FileCheck -check-prefix=SHADERTEST %s

// SHADERTEST-LABEL: {{^// LLPC}} final pipeline module info
// SHADERTEST: call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.image.atomic.add.flt.1d.f32.i16.v8i32(float 1.000000e+00, i16 0, <8 x i32> %{{[0-9]*}}, i32 0, i32 0)

// SHADERTEST-LABEL: {{^// LLPC}} final ELF info
// SHADERTEST: image_atomic_add_flt {{v[0-9]*}}, {{v[0-9]*}}, {{s[[0-9]*:[0-9]*]}} dmask:0x1 dim:SQ_RSRC_IMG_1D th:TH_ATOMIC_RETURN a16

#version 450 core
#extension GL_EXT_shader_atomic_float: enable

layout(binding = 0, r32f) uniform image1D i1D;

void main()
{
   imageAtomicAdd(i1D, 0, 1.0);
}
