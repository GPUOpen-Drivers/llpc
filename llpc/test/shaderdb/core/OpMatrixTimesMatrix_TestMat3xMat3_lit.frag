#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) in vec3 l0 ;
layout(location = 1) in vec3 l1 ;
layout(location = 2) in vec3 l2 ;
layout(location = 3) in vec3 l3 ;


layout(location = 0) out vec4 color;

void main()
{
    mat3 x = mat3(l0, l1, l2);
    mat3 y = mat3(l1, l2, l3);
    mat3 z = x * y;
    color.xyz = z[0] + z[1] + z[2];
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: [3 x <3 x float>] (...) @lgc.create.matrix.times.matrix.a3v3f32

; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: fmul {{.*}}float %{{[^, ]*}}, %{{[^, ]*}}
; SHADERTEST: fadd {{.*}}float %{{[^, ]*}}, %{{[^, ]*}}

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
