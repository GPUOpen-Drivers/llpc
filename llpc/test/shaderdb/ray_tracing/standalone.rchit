/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST: _chit1:
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 460
#extension GL_EXT_ray_tracing : enable

void main()
{
}
