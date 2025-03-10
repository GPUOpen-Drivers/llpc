#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/



layout(xfb_buffer = 0, xfb_offset = 16, xfb_stride = 32, location = 0) out vec4 output1;

void main()
{
    gl_Position = vec4(1);
    output1 = vec4(2);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void (...) @lgc.write.xfb.output(i32 0, i32 16, i32 0, <4 x float> {{(splat \(float 2\.000000e\+00\))|(<float 2\.000000e\+00, float 2\.000000e\+00, float 2\.000000e\+00, float 2\.000000e\+00>)}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
