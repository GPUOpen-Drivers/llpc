/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/

// NOTE: Assertions have been autogenerated by tool/update_llpc_test_checks.py
// RUN: amdllpc -emit-lgc -o - %s | FileCheck -check-prefix=CHECK %s
#version 450 core

layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;
layout(location = 2) in vec4 c;

layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 f = fma(a, b, c);
    frag_color = f + fma(vec4(0.7), vec4(0.2), vec4(0.1));
}
// CHECK-LABEL: @lgc.shader.FS.main(
// CHECK-NEXT:  .entry:
// CHECK-NEXT:    [[TMP0:%.*]] = call <4 x float> (...) @lgc.create.read.generic.input.v4f32(i32 2, i32 0, i32 0, i32 0, i32 16, i32 poison)
// CHECK-NEXT:    [[TMP1:%.*]] = call <4 x float> (...) @lgc.create.read.generic.input.v4f32(i32 1, i32 0, i32 0, i32 0, i32 16, i32 poison)
// CHECK-NEXT:    [[TMP2:%.*]] = call <4 x float> (...) @lgc.create.read.generic.input.v4f32(i32 0, i32 0, i32 0, i32 0, i32 16, i32 poison)
// CHECK-NEXT:    [[TMP3:%.*]] = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.fma.v4f32(<4 x float> [[TMP2]], <4 x float> [[TMP1]], <4 x float> [[TMP0]])
// CHECK-NEXT:    [[TMP4:%.*]] = call reassoc nnan nsz arcp contract afn <4 x float> (...) @lgc.create.fma.v4f32(<4 x float> {{(splat \(float 0x3FE6666660000000\))|(<float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000>)}}, <4 x float> {{(splat \(float 0x3FC99999A0000000\))|(<float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000>)}}, <4 x float> {{(splat \(float 0x3FB99999A0000000\))|(<float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>)}})
// CHECK-NEXT:    [[TMP5:%.*]] = fadd reassoc nnan nsz arcp contract afn <4 x float> [[TMP3]], [[TMP4]]
// CHECK-NEXT:    call void (...) @lgc.create.write.generic.output(<4 x float> [[TMP5]], i32 0, i32 0, i32 0, i32 0, i32 0, i32 poison)
// CHECK-NEXT:    ret void
//
