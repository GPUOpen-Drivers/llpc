#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(binding = 0) uniform Uniforms
{
    ivec3 i3;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 f3 = intBitsToFloat(i3);

    fragColor = (f3.x != f3.y) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = bitcast <3 x i32> %{{.*}} to <3 x float>
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: load <3 x float>,
; SHADERTEST-LABEL: {{^// LLPC}} LGC lowering results
; SHADERTEST: %{{[a-z]*}} = bitcast <2 x i32> %{{.*}} to <2 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
