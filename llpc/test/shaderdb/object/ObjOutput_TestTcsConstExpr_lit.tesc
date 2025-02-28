#version 450 core
/* Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved. */


layout(vertices = 3) out;

layout(location = 0) patch out vec3 outColor;

void PatchOut(float f)
{
    outColor.x = 0.7;
    outColor[1] = f;
    outColor.z = outColor.y;
}

void main(void)
{
    PatchOut(4.5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f32(i32 0, i32 0, i32 0, i32 -1, float 0x3FE6666660000000)
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f32(i32 0, i32 0, i32 1, i32 -1, float 4.500000e+00)
; SHADERTEST: call float @lgc.output.import.generic__f32{{.*}}(i1 true, i32 0, i32 0, i32 1, i32 poison)
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f32(i32 0, i32 0, i32 2, i32 -1, float %{{[0-9]*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
