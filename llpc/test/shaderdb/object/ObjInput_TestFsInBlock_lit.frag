#version 450

layout(location = 2) in Block
{
    flat   int  i1;
    smooth vec3 f3;
    smooth mat4 m4;
} block;

layout(binding = 0) uniform Uniforms
{
    int i;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f = vec4(block.i1);
    f += vec4(block.f3, 1.0);
    f += block.m4[i];

    fragColor = f;
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-DAG: call i32 (...) @lgc.input.import.interpolated.{{.*}}
; SHADERTEST-DAG: call <3 x float> (...) @lgc.input.import.interpolated.v3f32{{.*}}
; SHADERTEST-DAG: call <4 x float> (...) @lgc.input.import.interpolated.v4f32{{.*}}
; SHADERTEST-DAG: call <4 x float> (...) @lgc.input.import.interpolated.v4f32{{.*}}
; SHADERTEST-DAG: call <4 x float> (...) @lgc.input.import.interpolated.v4f32{{.*}}
; SHADERTEST-DAG: call <4 x float> (...) @lgc.input.import.interpolated.v4f32{{.*}}
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.mov
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p1
; SHADERTEST-DAG: call float @llvm.amdgcn.interp.p2
; SHADERTEST-LABEL: {{^// LLPC}} final pipeline module info
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
