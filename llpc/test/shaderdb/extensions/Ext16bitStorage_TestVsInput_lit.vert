#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


#extension GL_AMD_gpu_shader_half_float: enable
#extension GL_AMD_gpu_shader_int16: enable

layout(location = 0) in f16vec3 f16v3;
layout(location = 1) in float16_t f16v1;

layout(location = 2) in i16vec3 i16v3;
layout(location = 3) in uint16_t u16v1;

void main()
{
    vec3  fv3 = f16v3;
    float fv1 = f16v1;

    fv3 += vec3(i16v3);
    fv1 += float(u16v1);

    gl_Position = vec4(fv3, fv1);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-DAG: call i16 @lgc.load.vertex.input__i16{{.*}}
; SHADERTEST-DAG: call <3 x i16> @lgc.load.vertex.input__v3i16{{.*}}
; SHADERTEST-DAG: call half @lgc.load.vertex.input__f16{{.*}}
; SHADERTEST-DAG: call <3 x half> @lgc.load.vertex.input__v3f16{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
