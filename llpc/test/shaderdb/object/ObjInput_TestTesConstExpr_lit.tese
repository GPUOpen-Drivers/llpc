#version 450 core
/* Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved. */


layout(triangles) in;

layout(location = 0) out float outColor;

float TessCoord()
{
    float f = gl_TessCoord.x;
    f += gl_TessCoord[1] * 0.5;
    f -= gl_TessCoord.z;
    return f;
}

void main(void)
{
    outColor = TessCoord();
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST-COUNT-3: call float @lgc.input.import.builtin.TessCoord.f32{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
