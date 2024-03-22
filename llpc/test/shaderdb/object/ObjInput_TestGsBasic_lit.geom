#version 450 core

layout(triangles) in;
layout(triangle_strip, max_vertices = 16) out;

layout(location = 2) in dvec4 inData1[];
layout(location = 5) in float inData2[];

void main()
{
    for (int i = 0; i < gl_in.length(); ++i)
    {
        gl_Position.x = float(inData1[i].z / inData1[i].y);
        gl_Position.y = inData2[i];

        EmitVertex();
    }

    EndPrimitive();
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-COUNT-3: call float @lgc.input.import.generic__f32{{.*}}
; SHADERTEST-COUNT-3: call <4 x double> @lgc.input.import.generic__v4f64{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
