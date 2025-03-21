#version 450
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

#extension GL_EXT_nonuniform_qualifier : require
layout(set=0,binding=0) uniform sampler2D samp2Ds[];
layout(set=0,binding=1) uniform sampler2D samp2D;
layout(set=0,binding=2) uniform sampler samp;
layout(set=0,binding=3) uniform texture2D image;
layout(set=0,binding=4) uniform sampler samps[];
layout(set=0,binding=5) uniform texture2D images[];

layout(location = 0) out vec4     FragColor;
layout(location = 0) in flat int  index1;
layout(location = 1) in flat int  index2;

void main()
{
  vec4 color1 = vec4(0);
  color1 += texture(samp2D, vec2(0, 0));
  color1 += texture(sampler2D(image, samp), vec2(0,0));
  color1 += texture(samp2Ds[0], vec2(0,0));
  color1 += texture(sampler2D(images[index1], samps[index2]), vec2(0, 0));

  color1 += texture(nonuniformEXT(sampler2D(images[index1], samp)), vec2(0,0));

  FragColor = color1;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512,
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512,
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 512,
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 896,
; SHADERTEST: call {{.*}} @lgc.create.image.sample.v4f32(i32 1, i32 536,
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: call <8 x i32> @llvm.amdgcn.readfirstlane.v8i32
; SHADERTEST: call <4 x i32> @llvm.amdgcn.readfirstlane.v4i32
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: {{%[0-9]*}} = call float @llvm.amdgcn.interp.mov
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
