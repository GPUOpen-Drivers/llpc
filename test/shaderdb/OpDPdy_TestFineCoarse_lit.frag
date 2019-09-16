#version 450

layout (location = 0) out vec4 fragColor;
layout (location = 1) in vec3 fsIn;

void main()
{
    vec3 f3 = fsIn;
    f3 = dFdy(f3);
    f3 = dFdyFine(f3);
    f3 = dFdyCoarse(f3);

    fragColor = (f3[0] == f3[1]) ? vec4(1.0) : vec4(0.5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call <3 x float> (...) @llpc.call.derivative.v3f32(<3 x float> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call <3 x float> (...) @llpc.call.derivative.v3f32(<3 x float> %{{.*}}, i1 true, i1 true)
; SHADERTEST: = call <3 x float> (...) @llpc.call.derivative.v3f32(<3 x float> %{{.*}}, i1 true, i1 false)
; SHADERTEST-LABEL: {{^// LLPC.*}} patching results
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32(i32 {{[%0-9]+}}, i32 170, i32 15, i32 15, i1 true)
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32(i32 {{[%0-9]+}}, i32 0, i32 15, i32 15, i1 true)
; SHADERTEST: call i32 @llvm.amdgcn.wqm.i32(i32 {{[%0-9]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
