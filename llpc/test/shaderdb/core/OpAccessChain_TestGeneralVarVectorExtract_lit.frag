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


layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform Uniforms
{
    int index;
};

void main()
{
    dvec3 d3 = dvec3(0.0);
    d3[index] = 0.5;
    d3[2] = 1.0;

    vec3 f3 = vec3(0.0);
    f3[index] = 2.0;

    double d1 = d3[1];
    float  f1 = f3[index] + f3[1];

    fragColor = vec4(float(d1), f1, f1, f1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s


; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: getelementptr <3 x double>, ptr addrspace({{.*}}) %{{.*}}, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: getelementptr <3 x float>, ptr addrspace({{.*}}) %{{.*}}, i32 0, i32 %{{[0-9]*}}
; SHADERTEST: getelementptr <3 x float>, ptr addrspace({{.*}}) %{{.*}}, i32 0, i32 %{{[0-9]*}}

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: %[[cmp1:.*]] = icmp eq i32 %{{[0-9]*}}, 1
; SHADERTEST: select i1 %[[cmp1]], i32 {{.*}}, i32 {{.*}}
; SHADERTEST: %[[cmp2:.*]] = icmp eq i32 %{{[0-9]*}}, 2
; SHADERTEST: select i1 %[[cmp2]], i32 {{.*}}, i32 {{.*}}

; SHADERTEST: %[[cmp11:.*]] = icmp eq i32 %{{[0-9]*}}, 1
; SHADERTEST: select i1 %[[cmp11]], i32 {{.*}}, i32 {{.*}}
; SHADERTEST: %[[cmp21:.*]] = icmp eq i32 %{{[0-9]*}}, 2
; SHADERTEST: select i1 %[[cmp21]], i32 {{.*}}, i32 {{.*}}

; SHADERTEST: %[[cmp12:.*]] = icmp eq i32 %{{[0-9]*}}, 1
; SHADERTEST: select i1 %[[cmp12]], float %{{.*}}, float %{{.*}}
; SHADERTEST: %[[cmp22:.*]] = icmp eq i32 %{{[0-9]*}}, 2
; SHADERTEST: select i1 %[[cmp22]], float %{{.*}}, float %{{[0-9]*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
