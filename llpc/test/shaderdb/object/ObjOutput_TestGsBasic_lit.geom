#version 450 core
/* Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved. */


layout(triangles) in;
layout(triangle_strip, max_vertices = 16) out;

layout(location = 2) out dvec4 outData1;
layout(location = 5) out float outData2;

void main()
{
    for (int i = 0; i < gl_in.length(); ++i)
    {
        outData1 = dvec4(gl_InvocationID);
        outData2 = float(gl_PrimitiveIDIn);

        EmitVertex();
    }

    EndPrimitive();
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v4f64
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
