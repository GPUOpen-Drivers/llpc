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


#extension GL_AMD_gpu_shader_half_float: enable

layout(binding = 0, std430) buffer Buffers
{
    vec3 fv3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    f16vec4 f16v4 = f16vec4(fv3, fv3.x);
    f16vec3 f16v3 = f16vec3(fv3);
    f16vec2 f16v2 = f16v3.yz;

    f16mat2x3 f16m2x3 = outerProduct(f16v3, f16v2);
    f16m2x3 = matrixCompMult(f16m2x3, f16m2x3);
    f16mat3x2 f16m3x2 = transpose(f16m2x3);

    f16mat2 f16m2 = f16mat2(f16v2, f16v2);
    f16mat3 f16m3 = f16mat3(f16v3, f16v3, f16v3);
    f16mat4 f16m4 = f16mat4(f16v4, f16v4, f16v4, f16v4);

    float16_t f16 = determinant(f16m2);
    f16 += determinant(f16m3);
    f16 += determinant(f16m4);

    f16m2 = inverse(f16m2);
    f16m3 = inverse(f16m3);
    f16m4 = inverse(f16m4);

    f16 += f16m3x2[1][1] + f16m2[1][1] + f16m3[1][1] + f16m4[1][1];

    fragColor = vec3(f16);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call {{.*}}[2 x <3 x half>] (...) @lgc.create.outer.product.a2v3f16(<3 x half> %{{.*}}, <2 x half> %{{.*}})
; SHADERTEST: %{{[^, ]*}} = call {{.*}}[3 x <2 x half>] {{.*}}@lgc.create.transpose.matrix.a3v2f16([2 x <3 x half>] %{{[^, ]*}})
; SHADERTEST: = call reassoc nnan nsz arcp contract afn half (...) @lgc.create.determinant.f16([2 x <2 x half>] %
; SHADERTEST: = call reassoc nnan nsz arcp contract afn half (...) @lgc.create.determinant.f16([3 x <3 x half>] %
; SHADERTEST: = call reassoc nnan nsz arcp contract afn half (...) @lgc.create.determinant.f16([4 x <4 x half>] %
; SHADERTEST: = call {{.*}}[2 x <2 x half>] (...) @lgc.create.matrix.inverse.a2v2f16([2 x <2 x half>] %
; SHADERTEST: = call {{.*}}[3 x <3 x half>] (...) @lgc.create.matrix.inverse.a3v3f16([3 x <3 x half>] %
; SHADERTEST: = call {{.*}}[4 x <4 x half>] (...) @lgc.create.matrix.inverse.a4v4f16([4 x <4 x half>] %
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
