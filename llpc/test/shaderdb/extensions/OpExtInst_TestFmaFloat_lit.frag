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

// NOTE: Assertions have been autogenerated by tool/update_llpc_test_checks.py
// RUN: amdllpc -emit-lgc -o - %s | FileCheck -check-prefix=CHECK %s
#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1, f1_2, f1_3;
    vec3 f3_1, f3_2, f3_3;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = fma(f1_1, f1_2, f1_3);

    vec3 f3_0 = fma(f3_1, f3_2, f3_3);

    fragColor = (f3_0.x != f1_0) ? vec4(0.0) : vec4(1.0);
}
// CHECK-LABEL: @lgc.shader.FS.main(
// CHECK-NEXT:  .entry:
// CHECK-NEXT:    [[TMP0:%.*]] = call ptr addrspace(7) @lgc.load.buffer.desc(i64 0, i32 0, i32 0, i32 0)
// CHECK-NEXT:    [[TMP1:%.*]] = call ptr @llvm.invariant.start.p7(i64 -1, ptr addrspace(7) [[TMP0]])
// CHECK-NEXT:    [[TMP2:%.*]] = load float, ptr addrspace(7) [[TMP0]], align 4
// CHECK-NEXT:    [[TMP3:%.*]] = getelementptr {{inbounds i8|<{ float, float, float, [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float] }>|i8}}, ptr addrspace(7) [[TMP0]], i32 {{4|0, i32 1}}
// CHECK-NEXT:    [[TMP4:%.*]] = load float, ptr addrspace(7) [[TMP3]], align 4
// CHECK-NEXT:    [[TMP5:%.*]] = getelementptr {{inbounds i8|<{ float, float, float, [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float] }>|i8}}, ptr addrspace(7) [[TMP0]], i32 {{8|0, i32 2}}
// CHECK-NEXT:    [[TMP6:%.*]] = load float, ptr addrspace(7) [[TMP5]], align 4
// CHECK-NEXT:    [[TMP7:%.*]] = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.fma.f32(float [[TMP2]], float [[TMP4]], float [[TMP6]])
// CHECK-NEXT:    [[TMP8:%.*]] = getelementptr {{inbounds i8|<{ float, float, float, [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float] }>|i8}}, ptr addrspace(7) [[TMP0]], i32 {{16|0, i32 4}}
// CHECK-NEXT:    [[TMP9:%.*]] = load <3 x float>, ptr addrspace(7) [[TMP8]], align 16
// CHECK-NEXT:    [[TMP10:%.*]] = getelementptr {{inbounds i8|<{ float, float, float, [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float] }>|i8}}, ptr addrspace(7) [[TMP0]], i32 {{32|0, i32 6}}
// CHECK-NEXT:    [[TMP11:%.*]] = load <3 x float>, ptr addrspace(7) [[TMP10]], align 16
// CHECK-NEXT:    [[TMP12:%.*]] = getelementptr {{inbounds i8|<{ float, float, float, [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float], [[]4 x i8], [[]3 x float] }>|i8}}, ptr addrspace(7) [[TMP0]], i32 {{48|0, i32 8}}
// CHECK-NEXT:    [[TMP13:%.*]] = load <3 x float>, ptr addrspace(7) [[TMP12]], align 16
// CHECK-NEXT:    [[TMP14:%.*]] = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.fma.v3f32(<3 x float> [[TMP9]], <3 x float> [[TMP11]], <3 x float> [[TMP13]])
// CHECK-NEXT:    [[F3_0_0_VEC_EXTRACT:%.*]] = extractelement <3 x float> [[TMP14]], i64 0
// CHECK-NEXT:    [[TMP15:%.*]] = fcmp une float [[F3_0_0_VEC_EXTRACT]], [[TMP7]]
// CHECK-NEXT:    [[TMP16:%.*]] = select reassoc nnan nsz arcp contract afn i1 [[TMP15]], <4 x float> zeroinitializer, <4 x float> {{(splat \(float 1\.000000e\+00\))|(<float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00>)}}
// CHECK-NEXT:    call void (...) @lgc.create.write.generic.output(<4 x float> [[TMP16]], i32 0, i32 0, i32 0, i32 0, i32 0, i32 poison)
// CHECK-NEXT:    ret void
//
