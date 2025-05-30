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

// NOTE: Assertions have been autogenerated by tool/update_llpc_test_checks.py UTC_ARGS: --function-signature --check-globals
// RUN: amdllpc -o - -gfxip 10.1 -emit-lgc %s | FileCheck -check-prefixes=CHECK %s
#version 450

out gl_PerVertex
{
    vec4 gl_Position;
    layout(xfb_offset = 0) float gl_PointSize;
};

layout(location = 0) in vec4 pos;
layout(location = 1) in float pointSize;


void main()
{
    gl_Position = pos;
    gl_PointSize = pointSize;
}
// CHECK-LABEL: define {{[^@]+}}@lgc.shader.VS.main
// CHECK-SAME: () local_unnamed_addr #[[ATTR0:[0-9]+]] !spirv.ExecutionModel !11 !lgc.shaderstage !1 !lgc.xfb.state !12 {
// CHECK-NEXT:  .entry:
// CHECK-NEXT:    [[TMP0:%.*]] = call float @lgc.load.vertex.input__f32(i1 false, i32 1, i32 0, i32 0, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison)
// CHECK-NEXT:    [[TMP1:%.*]] = call <4 x float> @lgc.load.vertex.input__v4f32(i1 false, i32 0, i32 0, i32 0, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison)
// CHECK-NEXT:    call void (...) @lgc.create.write.builtin.output(<4 x float> [[TMP1]], i32 0, i32 0, i32 poison, i32 poison)
// CHECK-NEXT:    call void (...) @lgc.create.write.xfb.output(float [[TMP0]], i1 true, i32 1, i32 0, i32 4, i32 0, i32 0)
// CHECK-NEXT:    call void (...) @lgc.create.write.builtin.output(float [[TMP0]], i32 1, i32 0, i32 poison, i32 poison)
// CHECK-NEXT:    ret void
//
//.
// CHECK: attributes #[[ATTR0]] = { alwaysinline nounwind "denormal-fp-math-f32"="preserve-sign" }
// CHECK: attributes #[[ATTR1:[0-9]+]] = { nounwind willreturn memory(none) }
// CHECK: attributes #[[ATTR2:[0-9]+]] = { nodivergencesource nounwind }
//.
