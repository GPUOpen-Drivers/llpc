#version 450

layout(binding = 0) uniform Uniforms
{
    vec2 f2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    uint u1 = packHalf2x16(f2);

    fragColor = (u1 != 5) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = call {{.*}} i32 @_Z12packHalf2x16Dv2_f(<2 x float> %{{.*}})
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: %{{[0-9]*}} = bitcast <8 x i8> %{{.*}} to <2 x float>
; SHADERTEST: %{{[0-9]*}} = extractelement <2 x float> %{{.*}}, i32 1
; SHADERTEST: %{{[0-9]*}} = fptrunc float %{{.*}} to half
; SHADERTEST: %{{[0-9]*}} = bitcast half %{{.*}} to i16
; SHADERTEST: %{{[0-9]*}} = zext i16 %{{.*}} to i32
; SHADERTEST: %{{[0-9]*}} = shl nuw i32 %{{.*}}, 16
; SHADERTEST: %{{[0-9]*}} = extractelement <2 x float> %{{.*}}, i32 0
; SHADERTEST: %{{[0-9]*}} = fptrunc float %{{.*}} to half
; SHADERTEST: %{{[0-9]*}} = bitcast half %{{.*}} to i16
; SHADERTEST: %{{[0-9]*}} = zext i16 %{{.*}} to i32
; SHADERTEST: %{{[0-9]*}} = or i32 %{{.*}}, %{{.*}}
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
