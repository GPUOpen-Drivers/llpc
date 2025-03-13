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


layout(binding = 0) uniform Uniforms
{
    vec4 f4;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    uint u1 = packUnorm4x8(f4);

    fragColor = (u1 != 5) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[CLAMP:.*]] = call <4 x float> (...) @lgc.create.fclamp.v4f32(<4 x float> %{{.*}}, <4 x float> zeroinitializer, <4 x float> {{(splat \(float 1\.000000e\+00\))|(<float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00>)}})
; SHADERTEST: %[[SCALE:.*]] = fmul <4 x float> %[[CLAMP]], {{(splat \(float 2\.550000e\+02\))|(<float 2\.550000e\+02, float 2\.550000e\+02, float 2\.550000e\+02, float 2\.550000e\+02>)}}
; SHADERTEST: %[[RINT:.*]] = call <4 x float> @llvm.rint.v4f32(<4 x float> %[[SCALE]])
; SHADERTEST: %[[CONV:.*]] = fptoui <4 x float> %[[RINT]] to <4 x i8>
; SHADERTEST: = bitcast <4 x i8> %[[CONV]] to i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
