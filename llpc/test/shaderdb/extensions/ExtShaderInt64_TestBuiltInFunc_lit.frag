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


#extension GL_ARB_gpu_shader_int64 : enable

layout(std140, binding = 0) uniform Uniforms
{
    bvec3    b3;
    dvec3    d3;
    int64_t  i64;
    uint64_t u64;
    i64vec3  i64v3;
    u64vec3  u64v3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    i64vec3 i64v3_0 = abs(i64v3);
    i64v3_0 += sign(i64v3);

    u64vec3 u64v3_0 = min(u64v3, u64);
    i64v3_0 += min(i64v3, i64);

    u64v3_0 += max(u64v3, u64);
    i64v3_0 += max(i64v3, i64);

    u64v3_0 += clamp(u64v3_0, u64v3.x, u64);
    i64v3_0 += clamp(i64v3_0, i64v3.x, i64);

    u64v3_0 += mix(u64v3_0, u64v3, b3);
    i64v3_0 += mix(i64v3_0, i64v3, b3);

    u64v3_0 += doubleBitsToUint64(d3);
    i64v3_0 += doubleBitsToInt64(d3);

    dvec3 d3_0 = uint64BitsToDouble(u64v3);
    d3_0 += int64BitsToDouble(i64v3);

    fragColor = ((u64v3_0.x != 0) && (i64v3_0.y == 0)) ? vec3(d3_0) : vec3(d3);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call <3 x i64> (...) @lgc.create.sabs.v3i64(<3 x i64>
; SHADERTEST: call <3 x i64> (...) @lgc.create.ssign.v3i64(<3 x i64>

; SHADERTEST: = call <3 x i64> @llvm.umin.v3i64(<3 x i64> %{{[0-9]*}}, <3 x i64> %{{[0-9]*}})

; SHADERTEST: = call <3 x i64> @llvm.smin.v3i64(<3 x i64> %{{[0-9]*}}, <3 x i64> %{{[0-9]*}})

; SHADERTEST: = call <3 x i64> @llvm.umax.v3i64(<3 x i64> %{{[0-9]*}}, <3 x i64> %{{[0-9]*}})

; SHADERTEST: = call <3 x i64> @llvm.smax.v3i64(<3 x i64> %{{[0-9]*}}, <3 x i64> %{{[0-9]*}})

; SHADERTEST:  %[[UCLAMPMAX:.*]] = call <3 x i64> @llvm.umax.v3i64(<3 x i64> %{{[0-9]*}}, <3 x i64> %{{[0-9]*}})
; SHADERTEST:  = call <3 x i64> @llvm.umin.v3i64(<3 x i64> %{{[0-9]*}}, <3 x i64>  %[[UCLAMPMAX:.*]])

; SHADERTEST:  %[[SCLAMPMAX:.*]] = call <3 x i64> @llvm.smax.v3i64(<3 x i64> %{{[0-9]*}}, <3 x i64> %{{[0-9]*}})
; SHADERTEST:  = call <3 x i64> @llvm.smin.v3i64(<3 x i64> %{{[0-9]*}}, <3 x i64>  %[[SCLAMPMAX:.*]])

; SHADERTEST: bitcast <3 x double> %{{[0-9]*}} to <3 x i64>
; SHADERTEST: bitcast <3 x i64> %{{[0-9]*}} to <3 x double>
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-LABEL: {{^// LLPC}} LGC before-lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
