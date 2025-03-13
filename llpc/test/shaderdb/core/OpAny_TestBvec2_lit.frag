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
// RUN: amdllpc -o - -gfxip 10.1 -emit-lgc %s | FileCheck -check-prefixes=CHECK %s

#version 450

layout(binding = 0) uniform Uniforms
{
    bvec2 b2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    if (any(b2) == true)
    {
        color = vec4(1.0);
    }

    fragColor = color;
}
// CHECK-LABEL: @lgc.shader.FS.main(
// CHECK-NEXT:  .entry:
// CHECK-NEXT:    [[TMP0:%.*]] = call ptr addrspace(7) @lgc.load.buffer.desc(i64 0, i32 0, i32 0, i32 0)
// CHECK-NEXT:    [[TMP1:%.*]] = call ptr @llvm.invariant.start.p7(i64 -1, ptr addrspace(7) [[TMP0]])
// CHECK-NEXT:    [[TMP2:%.*]] = load <2 x i32>, ptr addrspace(7) [[TMP0]], align 8
// CHECK-NEXT:    [[TMP3:%.*]] = extractelement <2 x i32> [[TMP2]], i64 0
// CHECK-NEXT:    [[TMP4:%.*]] = extractelement <2 x i32> [[TMP2]], i64 1
// CHECK-NEXT:    [[TMP5:%.*]] = or i32 [[TMP3]], [[TMP4]]
// CHECK-NEXT:    [[DOTFR:%.*]] = freeze i32 [[TMP5]]
// CHECK-NEXT:    [[DOTNOT:%.*]] = icmp eq i32 [[DOTFR]], 0
// CHECK-NEXT:    [[SPEC_SELECT:%.*]] = select i1 [[DOTNOT]], <4 x float> {{(splat \(float 5\.000000e\-01\))|(<float 5\.000000e\-01, float 5\.000000e\-01, float 5\.000000e\-01, float 5\.000000e\-01>)}}, <4 x float> {{(splat \(float 1\.000000e\+00\))|(<float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00>)}}
// CHECK-NEXT:    call void (...) @lgc.create.write.generic.output(<4 x float> [[SPEC_SELECT]], i32 0, i32 0, i32 0, i32 0, i32 0, i32 poison)
// CHECK-NEXT:    ret void
//
