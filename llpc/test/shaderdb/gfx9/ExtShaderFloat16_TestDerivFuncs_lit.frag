#version 450

#extension GL_AMD_gpu_shader_half_float: enable

layout(binding = 0, std430) buffer Buffers
{
    vec3 fv3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    f16vec3 f16v3 = f16vec3(fv3);

    f16v3 = dFdx(f16v3);
    f16v3 = dFdy(f16v3);
    f16v3 = dFdxFine(f16v3);
    f16v3 = dFdyFine(f16v3);
    f16v3 = dFdxCoarse(f16v3);
    f16v3 = dFdyCoarse(f16v3);
    f16v3 = fwidth(f16v3);
    f16v3 = fwidthFine(f16v3);
    f16v3 = fwidthCoarse(f16v3);

    fragColor = f16v3;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 true)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 false, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.derivative.v3f16(<3 x half> %{{.*}}, i1 true, i1 false)
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.fabs.v3f16(
; SHADERTEST: = fadd reassoc nnan nsz arcp contract afn <3 x half>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
