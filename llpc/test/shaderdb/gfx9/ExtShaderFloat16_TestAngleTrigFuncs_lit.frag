#version 450 core

#extension GL_AMD_gpu_shader_half_float: enable

layout(binding = 0) buffer Buffers
{
    vec3 fv3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    f16vec3 f16v3_1 = f16vec3(fv3);
    f16vec3 f16v3_2 = f16vec3(fv3);

    f16v3_1 = radians(f16v3_1);
    f16v3_1 = degrees(f16v3_1);
    f16v3_1 = sin(f16v3_1);
    f16v3_1 = cos(f16v3_1);
    f16v3_1 = tan(f16v3_1);
    f16v3_1 = asin(f16v3_1);
    f16v3_1 = acos(f16v3_1);
    f16v3_1 = atan(f16v3_1, f16v3_2);
    f16v3_1 = atan(f16v3_1);
    f16v3_1 = sinh(f16v3_1);
    f16v3_1 = cosh(f16v3_1);
    f16v3_1 = tanh(f16v3_1);
    f16v3_1 = asinh(f16v3_1);
    f16v3_1 = acosh(f16v3_1);
    f16v3_1 = atanh(f16v3_1);

    fragColor = f16v3_1;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <3 x half> %{{.*}}, <half 0xH2478, half 0xH2478, half 0xH2478>
; SHADERTEST: = fmul reassoc nnan nsz arcp contract afn <3 x half> %{{.*}}, <half 0xH5329, half 0xH5329, half 0xH5329>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.sin.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> @llvm.cos.v3f16(
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.tan.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.asin.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.acos.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.atan2.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.sinh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.cosh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.tanh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.asinh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.acosh.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x half> (...) @lgc.create.atanh.v3f16(<3 x half>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
