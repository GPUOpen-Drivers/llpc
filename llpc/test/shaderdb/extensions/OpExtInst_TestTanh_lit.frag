#version 450

layout(binding = 0) uniform Uniforms
{
    float f1_1;
    vec3 f3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    float f1_0 = tanh(f1_1);

    vec3 f3_0 = tanh(f3_1);

    fragColor = (f1_0 != f3_0.x) ? vec4(0.5) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.tanh.f32(float
; SHADERTEST: = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.tanh.v3f32(<3 x float>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: %{{.*}} = call reassoc nnan nsz arcp contract afn float @llvm.fabs.f32(float %{{[0-9]*}})
; SHADERTEST: %{{.*}} = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0xC007154760000000
; SHADERTEST: %{{.*}} = call reassoc nnan nsz arcp contract afn float @llvm.exp2.f32(float %{{.*}})
; SHADERTEST: %{{.*}} = fadd reassoc nnan nsz arcp contract afn float %{{.*}}, 1.000000e+00
; SHADERTEST: %{{.*}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fdiv.fast(float 2.000000e+00, float %{{.*}})
; SHADERTEST: %{{.*}} = fsub reassoc nnan nsz arcp contract afn float 1.000000e+00, %{{.*}}
; SHADERTEST: %{{.*}} = call reassoc nnan nsz arcp contract afn float @llvm.copysign.f32(float %{{.*}}, float %{{.*}})
; SHADERTEST: %{{.*}} = call reassoc nnan nsz arcp contract afn float @llvm.fabs.f32(float %{{[0-9]*}})
; SHADERTEST: %{{.*}} = fmul reassoc nnan nsz arcp contract afn float %{{.*}}, 0xC007154760000000
; SHADERTEST: %{{.*}} = call reassoc nnan nsz arcp contract afn float @llvm.exp2.f32(float %{{.*}})
; SHADERTEST: %{{.*}} = fadd reassoc nnan nsz arcp contract afn float %{{.*}}, 1.000000e+00
; SHADERTEST: %{{.*}} = call reassoc nnan nsz arcp contract afn float @llvm.amdgcn.fdiv.fast(float 2.000000e+00, float %{{.*}})
; SHADERTEST: %{{.*}} = fsub reassoc nnan nsz arcp contract afn float 1.000000e+00, %{{.*}}
; SHADERTEST: %{{.*}} = call reassoc nnan nsz arcp contract afn float @llvm.copysign.f32(float %{{.*}}, float %{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
