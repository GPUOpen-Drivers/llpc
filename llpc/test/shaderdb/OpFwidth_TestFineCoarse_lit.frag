#version 450

layout (location = 0) out vec4 fragColor;

void main()
{
    vec3 f3 = vec3(0.0);
    f3 = fwidth(f3);
    f3 = fwidthFine(f3);
    f3 = fwidthCoarse(f3);

    fragColor = (f3[0] == f3[1]) ? vec4(1.0) : vec4(0.5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.fabs.v3f32(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.fabs.v3f32(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 false, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 true, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.fabs.v3f32(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.fabs.v3f32(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> %{{[^, ]+}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.fabs.v3f32(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.fabs.v3f32(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x float>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
