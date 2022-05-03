#version 450 core

layout(vertices = 3) out;

layout(location = 1) out vec4 outColor[];

void main (void)
{
    outColor[gl_InvocationID] = gl_in[gl_InvocationID].gl_Position;
    outColor[gl_InvocationID].x += gl_in[gl_InvocationID].gl_PointSize;
    outColor[gl_InvocationID].y += gl_in[gl_InvocationID].gl_ClipDistance[2];
    outColor[gl_InvocationID].z += gl_in[gl_InvocationID].gl_CullDistance[3];

    int value = gl_PatchVerticesIn + gl_PrimitiveID;
    outColor[gl_InvocationID].w += float(value);
    outColor[gl_InvocationID].w += gl_in[gl_InvocationID].gl_Position.z;
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call <4 x float> @lgc.input.import.builtin.Position.v4f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.builtin.PointSize.f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.builtin.ClipDistance.f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.builtin.CullDistance.f32{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.PatchVertices{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.PrimitiveId{{.*}}
; SHADERTEST: call i32 @lgc.input.import.builtin.InvocationId{{.*}}
; SHADERTEST: call float @lgc.input.import.builtin.Position.f32{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
