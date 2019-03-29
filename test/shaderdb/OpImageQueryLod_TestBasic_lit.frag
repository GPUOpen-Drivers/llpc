#version 450 core

layout(set = 0, binding = 0 ) uniform sampler1D samp1D;
layout(set = 0, binding = 1 ) uniform sampler2D samp2D;
layout(set = 0, binding = 2 ) uniform sampler3D samp3D;
layout(set = 0, binding = 3 ) uniform samplerCube sampCube;
layout(set = 0, binding = 4 ) uniform sampler1DArray samp1DA;
layout(set = 0, binding = 5 ) uniform sampler2DArray samp2DA;
layout(set = 0, binding = 6 ) uniform samplerCubeArray sampCubeA;
layout(set = 0, binding = 7 ) uniform sampler1DShadow samp1DS;
layout(set = 0, binding = 8 ) uniform sampler2DShadow samp2DS;
layout(set = 0, binding = 9 ) uniform samplerCubeShadow sampCubeS;
layout(set = 0, binding = 10) uniform sampler1DArrayShadow samp1DAS;
layout(set = 0, binding = 11) uniform sampler2DArrayShadow samp2DAS;
layout(set = 0, binding = 12) uniform samplerCubeArrayShadow sampCubeAS;

layout(location = 0) out vec4 oOut;

void main()
{
    vec2 temp = vec2(0);

    float coord1D = 7;
    vec2  coord2D = vec2(7, 8);
    vec3  coord3D = vec3(7, 8, 9);

    temp = textureQueryLod(samp1D, coord1D);
    temp += textureQueryLod(samp2D, coord2D);
    temp += textureQueryLod(samp3D, coord3D);
    temp += textureQueryLod(sampCube, coord3D);
    temp += textureQueryLod(samp1DA, coord1D);
    temp += textureQueryLod(samp2DA, coord2D);
    temp += textureQueryLod(sampCubeA, coord3D);
    temp += textureQueryLod(samp1DS, coord1D);
    temp += textureQueryLod(samp2DS, coord2D);
    temp += textureQueryLod(sampCubeS, coord3D);
    temp += textureQueryLod(samp1DAS, coord1D);
    temp += textureQueryLod(samp2DAS, coord2D);
    temp += textureQueryLod(sampCubeAS, coord3D);

    oOut = vec4(temp.x, temp.y, 0, 0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 0, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.1D{{.*}}({{.*}},{{.*}}, float 7.000000e+00,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 1, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.2D{{.*}}({{.*}},{{.*}}, <2 x float> <float 7.000000e+00, float 8.000000e+00>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 2, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 2, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.3D{{.*}}({{.*}},{{.*}}, <3 x float> <float 7.000000e+00, float 8.000000e+00, float 9.000000e+00>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 3, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 3, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.Cube{{.*}}({{.*}},{{.*}}, <3 x float> <float 7.000000e+00, float 8.000000e+00, float 9.000000e+00>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 4, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 4, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.1D{{.*}}({{.*}},{{.*}}, float 7.000000e+00,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 5, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 5, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.2D{{.*}}({{.*}},{{.*}}, <2 x float> <float 7.000000e+00, float 8.000000e+00>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 6, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 6, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.Cube{{.*}}({{.*}},{{.*}}, <3 x float> <float 7.000000e+00, float 8.000000e+00, float 9.000000e+00>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 7, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 7, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.1D{{.*}}({{.*}},{{.*}}, float 7.000000e+00,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 8, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 8, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.2D{{.*}}({{.*}},{{.*}}, <2 x float> <float 7.000000e+00, float 8.000000e+00>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 9, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 9, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.Cube{{.*}}({{.*}},{{.*}}, <3 x float> <float 7.000000e+00, float 8.000000e+00, float 9.000000e+00>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 10, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 10, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.1D{{.*}}({{.*}},{{.*}}, float 7.000000e+00,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 11, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 11, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.2D{{.*}}({{.*}},{{.*}}, <2 x float> <float 7.000000e+00, float 8.000000e+00>,{{.*}})
; SHADERTEST: call <4 x i32> {{.*}} @llpc.call.load.sampler.desc.v4i32(i32 0, i32 12, i32 0, i1 false)
; SHADERTEST: call <8 x i32> {{.*}} @llpc.call.load.resource.desc.v8i32(i32 0, i32 12, i32 0, i1 false)
; SHADERTEST: call <2 x float> @llpc.image.querylod.f32.Cube{{.*}}({{.*}},{{.*}}, <3 x float> <float 7.000000e+00, float 8.000000e+00, float 9.000000e+00>,{{.*}})

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.1d.v2f32.f32(i32 3, float 7.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.2d.v2f32.f32(i32 3, float 7.000000e+00, float 8.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.3d.v2f32.f32(i32 3, float 7.000000e+00, float 8.000000e+00, float 9.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.cube.v2f32.f32(i32 3,{{.*}},{{.*}},{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.1d.v2f32.f32(i32 3, float 7.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.2d.v2f32.f32(i32 3, float 7.000000e+00, float 8.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.cube.v2f32.f32(i32 3,{{.*}},{{.*}},{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.1d.v2f32.f32(i32 3, float 7.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.2d.v2f32.f32(i32 3, float 7.000000e+00, float 8.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.cube.v2f32.f32(i32 3,{{.*}},{{.*}},{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.1d.v2f32.f32(i32 3, float 7.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.2d.v2f32.f32(i32 3, float 7.000000e+00, float 8.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call <2 x float> @llvm.amdgcn.image.getlod.cube.v2f32.f32(i32 3,{{.*}},{{.*}},{{.*}},{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
