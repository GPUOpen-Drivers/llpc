/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/

// This test is to verify fragment outputs of array type.
// We must build color targets for all elements when enabling -auto-layout-desc.

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v --verify-ir %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 450

layout(location = 0) out vec4 o[8];
void main()
{
	o[0] = vec4(1.0, 0.0, 0.0, 1.0);
	o[2] = vec4(0.0, 1.0, 0.0, 1.0);
	o[7] = vec4(0.0, 0.0, 1.0, 1.0);
}
