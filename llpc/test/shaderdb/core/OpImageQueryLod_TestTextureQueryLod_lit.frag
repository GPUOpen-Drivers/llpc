#version 450

layout(set = 0, binding = 0) uniform sampler1D          samp1D;
layout(set = 1, binding = 0) uniform sampler2D          samp2D[4];
layout(set = 3, binding = 0) uniform texture3D          tex3D;
layout(set = 3, binding = 1) uniform sampler            samp;

layout(set = 4, binding = 0) uniform Uniforms
{
    int index;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 f2 = textureQueryLod(samp1D, 1.0);
    f2 += textureQueryLod(samp2D[index], vec2(0.5));
    f2 += textureQueryLod(sampler3D(tex3D, samp), vec3(0.7));

    fragColor = vec4(f2, f2);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}}  SPIRV-to-LLVM translation results
; 1D
; SHADERTEST: [[LOD1:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.create.image.get.lod.v2f32(i32 0, i32 512, {{.*}}, {{.*}}, float 1.000000e+00)
; SHADERTEST: [[DPX:%[0-9]*]] = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.derivative.f32(float 1.000000e+00, i1 false, i1 true)
; SHADERTEST: [[ABSDPX:%[0-9]*]] = call reassoc nnan nsz arcp contract afn float @llvm.fabs.f32(float [[DPX]])
; SHADERTEST: [[DPY:%[0-9]*]] = call reassoc nnan nsz arcp contract afn float (...) @lgc.create.derivative.f32(float 1.000000e+00, i1 true, i1 true)
; SHADERTEST: [[ABSDPY:%[0-9]*]] = call reassoc nnan nsz arcp contract afn float @llvm.fabs.f32(float [[DPY]])
; SHADERTEST: [[SUM:%[0-9]*]] = fadd reassoc nnan nsz arcp contract afn float [[ABSDPX]], [[ABSDPY]]
; SHADERTEST: [[ZERO:%[0-9]*]] = fcmp reassoc nnan nsz arcp contract afn oeq float [[SUM]], 0.000000e+00
; -FLT_MAX for LOD
; SHADERTEST: [[LOD2:%[0-9]*]] = insertelement <2 x float> [[LOD1]], float 0xC7EFFFFFE0000000, i64 1
; SHADERTEST: select reassoc nnan nsz arcp contract afn i1 [[ZERO]], <2 x float> [[LOD2]], <2 x float> [[LOD1]]

; 2D
; SHADERTEST: [[LOD1:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.create.image.get.lod.v2f32(i32 1, i32 384, {{.*}}, {{.*}}, <2 x float> <float 5.000000e-01, float 5.000000e-01>)
; SHADERTEST: [[DPX:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.create.derivative.v2f32(<2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 false, i1 true)
; SHADERTEST: [[ABSDPX:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <2 x float> @llvm.fabs.v2f32(<2 x float> [[DPX]])
; SHADERTEST: [[DPY:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.create.derivative.v2f32(<2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, i1 true)
; SHADERTEST: [[ABSDPY:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <2 x float> @llvm.fabs.v2f32(<2 x float> [[DPY]])
; SHADERTEST: [[SUM:%[0-9]*]] = fadd reassoc nnan nsz arcp contract afn <2 x float> [[ABSDPX]], [[ABSDPY]]
; SHADERTEST: [[ZERO:%[0-9]*]] = fcmp reassoc nnan nsz arcp contract afn oeq <2 x float> [[SUM]], zeroinitializer
; SHADERTEST: [[ZERO_X:%[0-9]*]] = extractelement <2 x i1> [[ZERO]], i64 0
; SHADERTEST: [[CMP1:%[0-9]*]] = and i1 [[ZERO_X]], true
; SHADERTEST: [[ZERO_Y:%[0-9]*]] = extractelement <2 x i1> [[ZERO]], i64 1
; SHADERTEST: [[CMP2:%[0-9]*]] = and i1 [[ZERO_Y]], [[CMP1]]
; SHADERTEST: [[LOD2:%[0-9]*]] = insertelement <2 x float> [[LOD1]], float 0xC7EFFFFFE0000000, i64 1
; SHADERTEST: select reassoc nnan nsz arcp contract afn i1 [[CMP2]], <2 x float> [[LOD2]], <2 x float> [[LOD1]]

; 3D
; SHADERTEST: [[LOD1:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <2 x float> (...) @lgc.create.image.get.lod.v2f32(i32 2, i32 512, {{.*}}, {{.*}}, <3 x float> <float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000>)
; SHADERTEST: [[DPX:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> <float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000>, i1 false, i1 true)
; SHADERTEST: [[ABSDPX:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.fabs.v3f32(<3 x float> [[DPX]])
; SHADERTEST: [[DPY:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <3 x float> (...) @lgc.create.derivative.v3f32(<3 x float> <float 0x3FE6666660000000, float 0x3FE6666660000000, float 0x3FE6666660000000>, i1 true, i1 true)
; SHADERTEST: [[ABSDPY:%[0-9]*]] = call reassoc nnan nsz arcp contract afn <3 x float> @llvm.fabs.v3f32(<3 x float> [[DPY]])
; SHADERTEST: [[SUM:%[0-9]*]] = fadd reassoc nnan nsz arcp contract afn <3 x float> [[ABSDPX]], [[ABSDPY]]
; SHADERTEST: [[ZERO:%[0-9]*]] =  fcmp reassoc nnan nsz arcp contract afn oeq <3 x float> [[SUM]], zeroinitializer
; SHADERTEST: [[ZERO_X:%[0-9]*]] = extractelement <3 x i1> [[ZERO]], i64 0
; SHADERTEST: [[CMP1:%[0-9]*]] = and i1 [[ZERO_X]], true
; SHADERTEST: [[ZERO_Y:%[0-9]*]] = extractelement <3 x i1> [[ZERO]], i64 1
; SHADERTEST: [[CMP2:%[0-9]*]] = and i1 [[ZERO_Y]], [[CMP1]]
; SHADERTEST: [[ZERO_Z:%[0-9]*]] = extractelement <3 x i1> [[ZERO]], i64 2
; SHADERTEST: [[CMP3:%[0-9]*]] = and i1 [[ZERO_Z]], [[CMP2]]
; SHADERTEST: [[LOD2:%[0-9]*]] = insertelement <2 x float> [[LOD1]], float 0xC7EFFFFFE0000000, i64 1
; SHADERTEST: select reassoc nnan nsz arcp contract afn i1 [[CMP3]], <2 x float> [[LOD2]], <2 x float> [[LOD1]]
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
