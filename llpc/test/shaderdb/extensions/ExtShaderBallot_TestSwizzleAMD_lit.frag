#version 450 core
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


#extension GL_AMD_shader_ballot: enable

layout(location = 0) out vec4 fv4Out;

layout(location = 0) in flat ivec2 iv2In;
layout(location = 1) in flat uvec3 uv3In;
layout(location = 2) in flat vec4  fv4In;

void main()
{
    vec4 fv4 = vec4(0.0);

    fv4.xy  += swizzleInvocationsAMD(iv2In, uvec4(0, 1, 2, 3));
    fv4.xyz += swizzleInvocationsAMD(uv3In, uvec4(3, 2, 1, 0));
    fv4     += swizzleInvocationsAMD(fv4In, uvec4(1, 0, 3, 2));

    fv4.xy  += swizzleInvocationsMaskedAMD(iv2In, uvec3(16, 2, 8));
    fv4.xyz += swizzleInvocationsMaskedAMD(uv3In, uvec3(14, 28, 7));
    fv4     += swizzleInvocationsMaskedAMD(fv4In, uvec3(3, 15, 6));

    fv4Out = fv4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call <2 x i32> (...) @lgc.create.subgroup.swizzle.quad.v2i32(<2 x i32> %{{[0-9]*}}, <4 x i32> <i32 0, i32 1, i32 2, i32 3>)
; SHADERTEST: call <3 x i32> (...) @lgc.create.subgroup.swizzle.quad.v3i32(<3 x i32> %{{[0-9]*}}, <4 x i32> <i32 3, i32 2, i32 1, i32 0>)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.subgroup.swizzle.quad.v4f32(<4 x float> %{{[0-9]*}}, <4 x i32> <i32 1, i32 0, i32 3, i32 2>)
; SHADERTEST: call <2 x i32> (...) @lgc.create.subgroup.swizzle.mask.v2i32(<2 x i32> %{{[0-9]*}}, <3 x i32> <i32 16, i32 2, i32 8>)
; SHADERTEST: call <3 x i32> (...) @lgc.create.subgroup.swizzle.mask.v3i32(<3 x i32> %{{[0-9]*}}, <3 x i32> <i32 14, i32 28, i32 7>)
; SHADERTEST: call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.subgroup.swizzle.mask.v4f32(<4 x float> %{{[0-9]*}}, <3 x i32> <i32 3, i32 15, i32 6>)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
