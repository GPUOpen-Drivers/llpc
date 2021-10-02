#version 450

layout (location = 0) out vec4 fragColor;
layout (location = 1) in vec3 fsIn;

void main()
{
    vec3 f3 = fsIn;
    f3 = dFdx(f3);
    f3 = dFdxFine(f3);
    f3 = dFdxCoarse(f3);

    fragColor = (f3[0] == f3[1]) ? vec4(1.0) : vec4(0.5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 false, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 false, i1 false)
; SHADERTEST-LABEL: {{^// LLPC.*}} patching results
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32(i32 {{[%0-9]+}}, i32 85, i32 15, i32 15, i1 true)
; SHADERTEST: call i32 @llvm.amdgcn.mov.dpp.i32(i32 {{[%0-9]+}}, i32 0, i32 15, i32 15, i1 true)
; SHADERTEST: call {{.*}}float @llvm.amdgcn.wqm.f32(float %{{[^) ]+}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
