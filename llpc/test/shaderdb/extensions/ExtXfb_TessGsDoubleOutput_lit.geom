#version 450 core
/* Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved. */


layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;

layout(location = 0) in dvec4 fIn[];
layout(location = 0, xfb_buffer = 1, xfb_offset = 24, stream = 0) out dvec3 fOut1;
layout(location = 2, xfb_buffer = 0, xfb_offset = 16, stream = 1) out dvec2 fOut2;

void main()
{
    for (int i = 0; i < gl_in.length(); ++i)
    {
        fOut1 = fIn[i].xyz;
        EmitStreamVertex(0);

        fOut2 = fIn[i].xy;
        EmitStreamVertex(1);
    }

    EndPrimitive();
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void (...) @lgc.write.xfb.output({{.*}}<3 x double>
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v3f64
; SHADERTEST: call void (...) @lgc.write.xfb.output({{.*}}<2 x double>
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v2f64
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
