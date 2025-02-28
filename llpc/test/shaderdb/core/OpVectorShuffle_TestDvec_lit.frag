/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/

// NOTE: Assertions have been autogenerated by tool/update_llpc_test_checks.py
// RUN: amdllpc -emit-lgc -gfxip 10.3 -o - %s | FileCheck -check-prefix=SHADERTEST %s

#version 450

layout(binding = 0) uniform Uniforms
{
    dvec3 d3;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 color = vec4(0.5);

    dvec4 d4_0 = dvec4(0), d4_1 = dvec4(0);

    d4_0.wz = d3.yx;
    d4_1.xw = d3.zz;

    if (d4_0 != d4_1)
    {
        color = vec4(1.0);
    }

    fragColor = color;
}

// SHADERTEST-LABEL: @lgc.shader.FS.main(
// SHADERTEST-NEXT:  .entry:
// SHADERTEST-NEXT:    [[TMP0:%.*]] = call ptr addrspace(7) @lgc.load.buffer.desc(i64 0, i32 0, i32 0, i32 0)
// SHADERTEST-NEXT:    [[TMP1:%.*]] = call ptr @llvm.invariant.start.p7(i64 -1, ptr addrspace(7) [[TMP0]])
// SHADERTEST-NEXT:    [[TMP2:%.*]] = load <3 x double>, ptr addrspace(7) [[TMP0]], align 32
// SHADERTEST-NEXT:    [[TMP3:%.*]] = extractelement <3 x double> [[TMP2]], i64 2
// SHADERTEST-NEXT:    [[TMP4:%.*]] = fcmp une double [[TMP3]], 0.000000e+00
// SHADERTEST-NEXT:    [[TMP5:%.*]] = extractelement <3 x double> [[TMP2]], i64 0
// SHADERTEST-NEXT:    [[TMP6:%.*]] = fcmp une double [[TMP5]], 0.000000e+00
// SHADERTEST-NEXT:    [[TMP7:%.*]] = extractelement <3 x double> [[TMP2]], i64 1
// SHADERTEST-NEXT:    [[TMP8:%.*]] = fcmp une double [[TMP7]], [[TMP3]]
// SHADERTEST-NEXT:    [[TMP9:%.*]] = or i1 [[TMP4]], [[TMP6]]
// SHADERTEST-NEXT:    [[TMP10:%.*]] = or i1 [[TMP9]], [[TMP8]]
// SHADERTEST-NEXT:    [[COND_FREEZE:%.*]] = freeze i1 [[TMP10]]
// SHADERTEST-NEXT:    [[SPEC_SELECT:%.*]] = select i1 [[COND_FREEZE]], <4 x float> {{(splat \(float 1\.000000e\+00\))|(<float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00, float 1\.000000e\+00>)}}, <4 x float> {{(splat \(float 5\.000000e\-01\))|(<float 5\.000000e\-01, float 5\.000000e\-01, float 5\.000000e\-01, float 5\.000000e\-01>)}}
// SHADERTEST-NEXT:    call void (...) @lgc.create.write.generic.output(<4 x float> [[SPEC_SELECT]], i32 0, i32 0, i32 0, i32 0, i32 0, i32 poison)
// SHADERTEST-NEXT:    ret void
//
