#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) in vec4 fIn;
layout(location = 0, xfb_buffer = 1, xfb_offset = 24) out vec3 fOut1;
layout(location = 1, xfb_buffer = 0, xfb_offset = 16) out vec2 fOut2;

void main()
{
    fOut1 = fIn.xyz;
    fOut2 = fIn.xy;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void (...) @lgc.write.xfb.output({{.*}}<3 x float>
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v3f32
; SHADERTEST: call void (...) @lgc.write.xfb.output({{.*}}<2 x float>
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v2f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
