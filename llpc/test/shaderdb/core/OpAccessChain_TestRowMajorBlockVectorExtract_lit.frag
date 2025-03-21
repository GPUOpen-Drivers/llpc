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
// RUN: amdllpc -gfxip 10.1 -emit-lgc -verify-ir -o - %s | FileCheck -check-prefix=SHADERTEST %s
// REQUIRES: do-not-run-me
#version 450

layout(set = 0, binding = 0, row_major) uniform DATA0
{
    dvec3   d3;
    dmat2x3 dm2x3;
} data0;

layout(set = 1, binding = 1, row_major) buffer DATA1
{
    vec4  f4;
    mat4  m4;
} data1;

layout(location = 0) out vec4 fragColor;

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
};

void main()
{
    double d1 = data0.d3[1];
    d1 += data0.d3[index];
    d1 += data0.dm2x3[1][1];
    d1 += data0.dm2x3[1][index];
    d1 += data0.dm2x3[index][1];
    d1 += data0.dm2x3[index + 1][index];

    float f1 = data1.f4[index];
    f1 += data1.f4[2];
    f1 += data1.m4[2][3];
    f1 += data1.m4[index][2];
    f1 += data1.m4[3][index];
    f1 += data1.m4[index][index + 1];

    fragColor = vec4(float(d1), f1, f1, f1);
}

// SHADERTEST-LABEL: @lgc.shader.FS.main(
// SHADERTEST-NEXT:  .entry:
// SHADERTEST-NEXT:    [[TMP0:%.*]] = call ptr addrspace(7) @lgc.load.buffer.desc(i64 1, i32 1, i32 0, i32 2)
// SHADERTEST-NEXT:    [[TMP1:%.*]] = call ptr addrspace(7) @lgc.load.buffer.desc(i64 2, i32 0, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP2:%.*]] = call ptr @llvm.invariant.start.p7(i64 -1, ptr addrspace(7) [[TMP1]])
// SHADERTEST-NEXT:    [[TMP3:%.*]] = call ptr addrspace(7) @lgc.load.buffer.desc(i64 0, i32 0, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP4:%.*]] = call ptr @llvm.invariant.start.p7(i64 -1, ptr addrspace(7) [[TMP3]])
// SHADERTEST-NEXT:    [[TMP5:%.*]] = getelementptr inbounds <{ [3 x double], [8 x i8], [3 x %llpc.matrix.row] }>, ptr addrspace(7) [[TMP3]], i64 0, i32 0, i64 1
// SHADERTEST-NEXT:    [[TMP6:%.*]] = load double, ptr addrspace(7) [[TMP5]], align 8
// SHADERTEST-NEXT:    [[TMP7:%.*]] = load i32, ptr addrspace(7) [[TMP1]], align 4
// SHADERTEST-NEXT:    [[TMP8:%.*]] = sext i32 [[TMP7]] to i64
// SHADERTEST-NEXT:    [[TMP9:%.*]] = getelementptr <{ [3 x double], [8 x i8], [3 x %llpc.matrix.row] }>, ptr addrspace(7) [[TMP3]], i64 0, i32 0, i64 [[TMP8]]
// SHADERTEST-NEXT:    [[TMP10:%.*]] = load double, ptr addrspace(7) [[TMP9]], align 8
// SHADERTEST-NEXT:    [[TMP11:%.*]] = fadd reassoc nnan nsz arcp contract double [[TMP6]], [[TMP10]]
// SHADERTEST-NEXT:    [[TMP12:%.*]] = getelementptr inbounds <{ [3 x double], [8 x i8], [3 x %llpc.matrix.row] }>, ptr addrspace(7) [[TMP3]], i64 0, i32 2, i64 1, i32 0, i64 1
// SHADERTEST-NEXT:    [[TMP13:%.*]] = load double, ptr addrspace(7) [[TMP12]], align 8
// SHADERTEST-NEXT:    [[TMP14:%.*]] = fadd reassoc nnan nsz arcp contract double [[TMP11]], [[TMP13]]
// SHADERTEST-NEXT:    [[TMP15:%.*]] = getelementptr <{ [3 x double], [8 x i8], [3 x %llpc.matrix.row] }>, ptr addrspace(7) [[TMP3]], i64 0, i32 2, i64 [[TMP8]], i32 0, i64 1
// SHADERTEST-NEXT:    [[TMP16:%.*]] = load double, ptr addrspace(7) [[TMP15]], align 8
// SHADERTEST-NEXT:    [[TMP17:%.*]] = fadd reassoc nnan nsz arcp contract double [[TMP14]], [[TMP16]]
// SHADERTEST-NEXT:    [[TMP18:%.*]] = getelementptr <{ [3 x double], [8 x i8], [3 x %llpc.matrix.row] }>, ptr addrspace(7) [[TMP3]], i64 0, i32 2, i64 1, i32 0, i64 [[TMP8]]
// SHADERTEST-NEXT:    [[TMP19:%.*]] = load double, ptr addrspace(7) [[TMP18]], align 8
// SHADERTEST-NEXT:    [[TMP20:%.*]] = fadd reassoc nnan nsz arcp contract double [[TMP17]], [[TMP19]]
// SHADERTEST-NEXT:    [[TMP21:%.*]] = add i32 [[TMP7]], 1
// SHADERTEST-NEXT:    [[TMP22:%.*]] = sext i32 [[TMP21]] to i64
// SHADERTEST-NEXT:    [[TMP23:%.*]] = getelementptr <{ [3 x double], [8 x i8], [3 x %llpc.matrix.row] }>, ptr addrspace(7) [[TMP3]], i64 0, i32 2, i64 [[TMP8]], i32 0, i64 [[TMP22]]
// SHADERTEST-NEXT:    [[TMP24:%.*]] = load double, ptr addrspace(7) [[TMP23]], align 8
// SHADERTEST-NEXT:    [[TMP25:%.*]] = fadd reassoc nnan nsz arcp contract double [[TMP20]], [[TMP24]]
// SHADERTEST-NEXT:    [[TMP26:%.*]] = getelementptr <{ [4 x float], [4 x %llpc.matrix.row.0] }>, ptr addrspace(7) [[TMP0]], i64 0, i32 0, i64 [[TMP8]]
// SHADERTEST-NEXT:    [[TMP27:%.*]] = load float, ptr addrspace(7) [[TMP26]], align 4
// SHADERTEST-NEXT:    [[TMP28:%.*]] = getelementptr inbounds <{ [4 x float], [4 x %llpc.matrix.row.0] }>, ptr addrspace(7) [[TMP0]], i64 0, i32 0, i64 2
// SHADERTEST-NEXT:    [[TMP29:%.*]] = load float, ptr addrspace(7) [[TMP28]], align 4
// SHADERTEST-NEXT:    [[TMP30:%.*]] = fadd reassoc nnan nsz arcp contract afn float [[TMP27]], [[TMP29]]
// SHADERTEST-NEXT:    [[TMP31:%.*]] = getelementptr inbounds <{ [4 x float], [4 x %llpc.matrix.row.0] }>, ptr addrspace(7) [[TMP0]], i64 0, i32 1, i64 3, i32 0, i64 2
// SHADERTEST-NEXT:    [[TMP32:%.*]] = load float, ptr addrspace(7) [[TMP31]], align 4
// SHADERTEST-NEXT:    [[TMP33:%.*]] = fadd reassoc nnan nsz arcp contract afn float [[TMP30]], [[TMP32]]
// SHADERTEST-NEXT:    [[TMP34:%.*]] = getelementptr <{ [4 x float], [4 x %llpc.matrix.row.0] }>, ptr addrspace(7) [[TMP0]], i64 0, i32 1, i64 2, i32 0, i64 [[TMP8]]
// SHADERTEST-NEXT:    [[TMP35:%.*]] = load float, ptr addrspace(7) [[TMP34]], align 4
// SHADERTEST-NEXT:    [[TMP36:%.*]] = fadd reassoc nnan nsz arcp contract afn float [[TMP33]], [[TMP35]]
// SHADERTEST-NEXT:    [[TMP37:%.*]] = getelementptr <{ [4 x float], [4 x %llpc.matrix.row.0] }>, ptr addrspace(7) [[TMP0]], i64 0, i32 1, i64 [[TMP8]], i32 0, i64 3
// SHADERTEST-NEXT:    [[TMP38:%.*]] = load float, ptr addrspace(7) [[TMP37]], align 4
// SHADERTEST-NEXT:    [[TMP39:%.*]] = fadd reassoc nnan nsz arcp contract afn float [[TMP36]], [[TMP38]]
// SHADERTEST-NEXT:    [[TMP40:%.*]] = getelementptr <{ [4 x float], [4 x %llpc.matrix.row.0] }>, ptr addrspace(7) [[TMP0]], i64 0, i32 1, i64 [[TMP22]], i32 0, i64 [[TMP8]]
// SHADERTEST-NEXT:    [[TMP41:%.*]] = load float, ptr addrspace(7) [[TMP40]], align 4
// SHADERTEST-NEXT:    [[TMP42:%.*]] = fadd reassoc nnan nsz arcp contract afn float [[TMP39]], [[TMP41]]
// SHADERTEST-NEXT:    [[TMP43:%.*]] = fptrunc double [[TMP25]] to float
// SHADERTEST-NEXT:    [[TMP44:%.*]] = insertelement <4 x float> undef, float [[TMP43]], i64 0
// SHADERTEST-NEXT:    [[TMP45:%.*]] = insertelement <4 x float> [[TMP44]], float [[TMP42]], i64 1
// SHADERTEST-NEXT:    [[TMP46:%.*]] = insertelement <4 x float> [[TMP45]], float [[TMP42]], i64 2
// SHADERTEST-NEXT:    [[TMP47:%.*]] = insertelement <4 x float> [[TMP46]], float [[TMP42]], i64 3
// SHADERTEST-NEXT:    call void (...) @lgc.create.write.generic.output(<4 x float> [[TMP47]], i32 0, i32 0, i32 0, i32 0, i32 0, i32 undef)
// SHADERTEST-NEXT:    ret void
//
